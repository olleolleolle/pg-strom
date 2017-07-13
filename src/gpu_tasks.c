/*
 * gputasks.c
 *
 * Routines to manage GpuTaskState/GpuTask state machine.
 * ----
 * Copyright 2011-2017 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2017 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "postgres.h"
#include "access/xact.h"
#include "optimizer/clauses.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "pg_strom.h"

/*
 * static functions
 */
static void pgstrom_accum_worker_statistics(GpuTaskState *gts);

/*
 * construct_kern_parambuf
 *
 * It construct a kernel parameter buffer to deliver Const/Param nodes.
 */
static kern_parambuf *
construct_kern_parambuf(List *used_params, ExprContext *econtext,
						List *custom_scan_tlist)
{
	StringInfoData	str;
	kern_parambuf  *kparams;
	char		padding[STROMALIGN_LEN];
	ListCell   *cell;
	Size		offset;
	int			index = 0;
	int			nparams = list_length(used_params);

	memset(padding, 0, sizeof(padding));

	/* seek to the head of variable length field */
	offset = STROMALIGN(offsetof(kern_parambuf, poffset[nparams]));
	initStringInfo(&str);
	enlargeStringInfo(&str, offset);
	memset(str.data, 0, offset);
	str.len = offset;
	/* walks on the Para/Const list */
	foreach (cell, used_params)
	{
		Node   *node = lfirst(cell);
		bool	nested_custom_scan_tlist = false;

	retry_custom_scan_tlist:
		if (IsA(node, Const))
		{
			Const  *con = (Const *) node;

			kparams = (kern_parambuf *)str.data;
			if (con->constisnull)
				kparams->poffset[index] = 0;	/* null */
			else if (con->constbyval)
			{
				Assert(con->constlen > 0);
				kparams->poffset[index] = str.len;
				appendBinaryStringInfo(&str,
									   (char *)&con->constvalue,
									   con->constlen);
			}
			else
			{
				kparams->poffset[index] = str.len;
				if (con->constlen > 0)
					appendBinaryStringInfo(&str,
										   DatumGetPointer(con->constvalue),
										   con->constlen);
				else
					appendBinaryStringInfo(&str,
                                           DatumGetPointer(con->constvalue),
                                           VARSIZE(con->constvalue));
			}
		}
		else if (IsA(node, Param))
		{
			ParamListInfo param_info = econtext->ecxt_param_list_info;
			Param  *param = (Param *) node;

			if (param_info &&
				param->paramid > 0 && param->paramid <= param_info->numParams)
			{
				ParamExternData	*prm = &param_info->params[param->paramid - 1];

				/* give hook a chance in case parameter is dynamic */
				if (!OidIsValid(prm->ptype) && param_info->paramFetch != NULL)
					(*param_info->paramFetch) (param_info, param->paramid);

				kparams = (kern_parambuf *)str.data;
				if (!OidIsValid(prm->ptype))
				{
					elog(INFO, "debug: Param has no particular data type");
					kparams->poffset[index++] = 0;	/* null */
					continue;
				}
				/* safety check in case hook did something unexpected */
				if (prm->ptype != param->paramtype)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("type of parameter %d (%s) does not match "
									"that when preparing the plan (%s)",
									param->paramid,
									format_type_be(prm->ptype),
									format_type_be(param->paramtype))));
				if (prm->isnull)
					kparams->poffset[index] = 0;	/* null */
				else
				{
					int16	typlen;
					bool	typbyval;

					get_typlenbyval(prm->ptype, &typlen, &typbyval);
					if (typbyval)
					{
						appendBinaryStringInfo(&str,
											   (char *)&prm->value,
											   typlen);
					}
					else if (typlen > 0)
					{
						appendBinaryStringInfo(&str,
											   DatumGetPointer(prm->value),
											   typlen);
					}
					else
					{
						appendBinaryStringInfo(&str,
											   DatumGetPointer(prm->value),
											   VARSIZE(prm->value));
					}
				}
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("no value found for parameter %d",
								param->paramid)));
			}
		}
		else if (!nested_custom_scan_tlist &&
				 IsA(node, Var) &&
				 custom_scan_tlist != NIL &&
				 ((Var *)node)->varno == INDEX_VAR &&
				 ((Var *)node)->varattno <= list_length(custom_scan_tlist))
		{
			/*
			 * NOTE: setrefs.c often replaces the Const/Param expressions on
			 * the @used_params, if custom_scan_tlist has an identical TLE.
			 * So, if expression is a references to the custom_scan_tlist,
			 * we try to solve the underlying value, then retry.
			 */
			AttrNumber		varattno = ((Var *)node)->varattno;
			TargetEntry	   *tle = list_nth(custom_scan_tlist, varattno - 1);

			node = (Node *)tle->expr;

			nested_custom_scan_tlist = true;
			goto retry_custom_scan_tlist;
		}
		else
			elog(ERROR, "unexpected node: %s", nodeToString(node));

		/* alignment */
		if (STROMALIGN(str.len) != str.len)
			appendBinaryStringInfo(&str, padding,
								   STROMALIGN(str.len) - str.len);
		index++;
	}
	Assert(STROMALIGN(str.len) == str.len);
	kparams = (kern_parambuf *)str.data;
	kparams->hostptr = (hostptr_t) &kparams->hostptr;
	kparams->xactStartTimestamp = GetCurrentTransactionStartTimestamp();
	kparams->length = str.len;
	kparams->nparams = nparams;

	return kparams;
}

/*
 * pgstromInitGpuTaskState
 */
void
pgstromInitGpuTaskState(GpuTaskState *gts,
						GpuContext *gcontext,
						GpuTaskKind task_kind,
						List *used_params,
						EState *estate)
{
	ExprContext	   *econtext = gts->css.ss.ps.ps_ExprContext;
	CustomScan	   *cscan = (CustomScan *)(gts->css.ss.ps.plan);

	gts->gcontext = gcontext;
	gts->task_kind = task_kind;
	gts->program_id = INVALID_PROGRAM_ID;	/* to be set later */
	gts->revision = 1;
	gts->kern_params = construct_kern_parambuf(used_params, econtext,
											   cscan->custom_scan_tlist);
	gts->scan_done = false;
	gts->row_format = false;

	gts->outer_bulk_exec = false;
	InstrInit(&gts->outer_instrument, estate->es_instrument);
	gts->scan_overflow = NULL;

	/*
	 * NOTE: initialization of HeapScanDesc was moved to the first try of
	 * ExecGpuXXX() call to support CPU parallel. A local HeapScanDesc shall
	 * be setup only when it is not responsible to partial read.
	 */

	/* callbacks shall be set by the caller */
	dlist_init(&gts->ready_tasks);
	gts->num_ready_tasks = 0;

	//XXX - might be obsolete
	gts->worker_stat = NULL;	/* setup by DSM init handler */
}

/*
 * fetch_next_gputask
 */
GpuTask *
fetch_next_gputask(GpuTaskState *gts)
{
	GpuContext		   *gcontext = gts->gcontext;
	SharedGpuContext   *shgcon = gcontext->shgcon;
	GpuTask			   *gtask;
	dlist_node		   *dnode;
	struct timeval		tv1, tv2;

	/*
	 * If no server connection is established, GpuTask cannot be processed
	 * by GPU devices. All we can do is CPU fallback instead of the GPU
	 * processing.
	 */
	if (gcontext->sockfd == PGINVALID_SOCKET)
	{
		gtask = gts->cb_next_task(gts);
		if (gtask)
			gtask->cpu_fallback = true;
		return gtask;
	}

retry_scan:
	CHECK_FOR_INTERRUPTS();
	while (gpuservRecvGpuTasks(gcontext, 0));

	/*
	 * Fetch next task and send it, if current resource consumption allows.
	 *
	 * 1. Number of the running tasks is less than pg_strom.min_async_tasks.
	 * 2. Sum of number of the running and ready tasks is less than
	 *    pg_strom.max_async_tasks.
	 */
	while (!gts->scan_done)
	{
		volatile uint32	num_server_gpu_tasks
			= GetNumberOfGpuServerTasks(gcontext->gpuserv_id);

		CHECK_FOR_INTERRUPTS();
		/*
		 * FIXME: We need to revise the GpuTask scheduler to utilize CPUs.
		 * Right now, it looks to me small fraction of CPU are utilized.
		 */
		SpinLockAcquire(&shgcon->lock);
		if (shgcon->num_async_tasks == 0 ||
			(num_server_gpu_tasks < pgstrom_max_async_tasks &&
			 gts->num_ready_tasks < 3))
		{
			SpinLockRelease(&shgcon->lock);

			/*
			 * TODO: If cb_next_task() couldn't set up GpuTask because of
			 * DMA buffer allocation, we may need to skip immediate load
			 * unless GTS has no running tasks.
			 */
			gtask = gts->cb_next_task(gts);
		  	if (!gtask)
		  	{
				gts->scan_done = true;
				break;
			}
#ifdef PGSTROM_DEBUG
			gettimeofday(&gtask->tv_timestamp, NULL);
#endif
			gpuservSendGpuTask(gcontext, gtask);
		}
		else
		{
			/* shouldn't send tasks any more at this moment */
			SpinLockRelease(&shgcon->lock);

			/* ok, we got at least one completed tasks */
			if (!dlist_is_empty(&gts->ready_tasks))
				break;
			/* wait for a completed task, or task vanish on server side */
			gettimeofday(&tv1, NULL);
			gpuservRecvGpuTasks(gcontext, -1);
			gettimeofday(&tv2, NULL);
			if (gts->task_kind == GpuTaskKind_GpuJoin)
				gcontext->debug_tv1 += ((1000000 * tv2.tv_sec + tv2.tv_usec) -
										(1000000 * tv1.tv_sec + tv1.tv_usec));
		}
		/* check completed tasks if any (non-blocking) */
		while (gpuservRecvGpuTasks(gcontext, 0));
	}

	/*
	 * Once we exit the above loop, either a completed task was returned,
	 * or relation scan has already done thus wait for synchronously.
	 */
	while (dlist_is_empty(&gts->ready_tasks))
	{
		Assert(gts->scan_done);
		CHECK_FOR_INTERRUPTS();
		SpinLockAcquire(&shgcon->lock);
		if (shgcon->num_async_tasks == 0)
		{
			SpinLockRelease(&shgcon->lock);
			return NULL;
		}
		SpinLockRelease(&shgcon->lock);
//		gettimeofday(&tv1, NULL);
		gpuservRecvGpuTasks(gcontext, -1);
//		gettimeofday(&tv2, NULL);
//		if (gts->task_kind == GpuTaskKind_GpuJoin)
//			gcontext->debug_tv2 += ((1000000 * tv2.tv_sec + tv2.tv_usec) -
//									(1000000 * tv1.tv_sec + tv1.tv_usec));
	}
	/* OK, pick up GpuTask from the head */
	Assert(gts->num_ready_tasks > 0);
	dnode = dlist_pop_head_node(&gts->ready_tasks);
	gtask = dlist_container(GpuTask, chain, dnode);
	gts->num_ready_tasks--;

	/*
	 * Discard GpuTask if revision number mismatch. ExecRescan() rewind the
	 * scan status then restart scan with new parameters. It means all the
	 * results of asynchronous tasks shall be discarded.
	 * To avoid synchronization here, all the GpuTask has a revision number
	 * copied from the GTS when it is constructed. It matched, the GpuTask
	 * is launched under the current GTS state.
	 */
	if (gtask->revision != gts->revision)
	{
		pgstromReleaseGpuTask(gtask);
		goto retry_scan;
	}

	gcontext->debug_tv2 += gtask->debug_delay;
	gcontext->count_tv2++;
	gcontext->debug_tv3 += gtask->send_delay;
	gcontext->count_tv3++;
	gcontext->debug_tv4 += gtask->kstart_delay;
	gcontext->count_tv4++;

	return gtask;
}

/*
 * pgstromExecGpuTaskState
 */
TupleTableSlot *
pgstromExecGpuTaskState(GpuTaskState *gts)
{
	TupleTableSlot *slot = gts->css.ss.ss_ScanTupleSlot;

	while (!gts->curr_task || !(slot = gts->cb_next_tuple(gts)))
	{
		GpuTask	   *gtask = gts->curr_task;

		/* release the current GpuTask object that was already scanned */
		if (gtask)
		{
			pgstromReleaseGpuTask(gtask);
			gts->curr_task = NULL;
			gts->curr_index = 0;
			gts->curr_lp_index = 0;
		}
		/* reload next chunk to be scanned */
		gtask = fetch_next_gputask(gts);
		if (!gtask)
			break;
		gts->curr_task = gtask;
		gts->curr_index = 0;
		gts->curr_lp_index = 0;
		/* notify a new task is assigned */
		if (gts->cb_switch_task)
			gts->cb_switch_task(gts, gtask);
	}
	return slot;
}

/*
 * pgstromBulkExecGpuTaskState
 */
pgstrom_data_store *
pgstromBulkExecGpuTaskState(GpuTaskState *gts, size_t chunk_size)
{
	pgstrom_data_store *pds_dst = NULL;
	TupleTableSlot	   *slot;

	/* GTS should not have neither host qualifier nor projection */
	Assert(gts->css.ss.ps.qual == NIL);
	Assert(gts->css.ss.ps.ps_ProjInfo == NULL);

	do {
		GpuTask	   *gtask = gts->curr_task;

		/* Reload next GpuTask to be scanned, if needed */
		if (!gtask)
		{
			gtask = fetch_next_gputask(gts);
			if (!gtask)
				break;	/* end of the scan */
			gts->curr_task = gtask;
			gts->curr_index = 0;
			gts->curr_lp_index = 0;
			if (gts->cb_switch_task)
				gts->cb_switch_task(gts, gtask);
		}
		Assert(gtask != NULL);

		while ((slot = gts->cb_next_tuple(gts)) != NULL)
		{
			/*
			 * Creation of the destination store on demand.
			 */
			if (!pds_dst)
			{
				pds_dst = PDS_create_row(gts->gcontext,
										 slot->tts_tupleDescriptor,
										 chunk_size);
			}

			/*
			 * Move rows from the source data-store to the destination store
			 * until:
			 *  The destination store still has space.
			 *  The source store still has unread rows.
			 */
			if (!PDS_insert_tuple(pds_dst, slot))
			{
				/* Rewind the source PDS, if destination gets filled up */
				Assert(gts->curr_index > 0 && gts->curr_lp_index == 0);
				gts->curr_index--;

				/*
				 * At least one tuple can be stored, unless the supplied
				 * chunk_size is not too small.
				 */
				if (pds_dst->kds.nitems == 0)
				{
					HeapTuple	tuple = ExecFetchSlotTuple(slot);
					elog(ERROR,
						 "Bug? Too short chunk_size (%zu) for tuple (len=%u)",
						 chunk_size, tuple->t_len);
				}
				return pds_dst;
			}
		}

		/*
		 * All the rows in pds_src are already fetched,
		 * so current GpuTask shall be detached.
		 */
		pgstromReleaseGpuTask(gtask);
		gts->curr_task = NULL;
		gts->curr_index = 0;
		gts->curr_lp_index = 0;
	} while (true);

	return pds_dst;
}

/*
 * pgstromRescanGpuTaskState
 */
void
pgstromRescanGpuTaskState(GpuTaskState *gts)
{
	/*
	 * Once revision number of GTS is changed, any asynchronous GpuTasks
	 * are discarded when 
	 *
	 */
	gts->revision++;
}

/*
 * pgstromReleaseGpuTaskState
 */
void
pgstromReleaseGpuTaskState(GpuTaskState *gts)
{
	/*
	 * collect perfmon statistics if parallel worker
	 *
	 * NOTE: ExplainCustomScan() shall be called prior to EndCustomScan() of
	 * the leader process. Thus, collection of the worker's statistics keeps
	 * ones by the workers, but no statistics come from the master backend.
	 * In addition, ExecEndGather() releases parallel context and DSM segment
	 * prior to EndCustomScan(), so any reference to @worker_state on the
	 * leader process will raise SEGV.
	 */
	if (ParallelMasterBackendId != InvalidBackendId)
		pgstrom_accum_worker_statistics(gts);
	else if (gts->worker_stat)
	{
		/* worker's statistics are no longer needed */
		dmaBufferFree(gts->worker_stat);
		gts->worker_stat = NULL;
	}
	/* cleanup per-query PDS-scan state, if any */
	PDS_end_heapscan_state(gts);
	/* release scan-desc if any */
	if (gts->css.ss.ss_currentScanDesc)
		heap_endscan(gts->css.ss.ss_currentScanDesc);
	/* unreference CUDA program */
	if (gts->program_id != INVALID_PROGRAM_ID)
		pgstrom_put_cuda_program(gts->gcontext, gts->program_id);

	/* unreference GpuContext */
	PutGpuContext(gts->gcontext);
}

/*
 * pgstromExplainGpuTaskState
 */
void
pgstromExplainGpuTaskState(GpuTaskState *gts, ExplainState *es)
{
	StringInfoData	buf;

	/*
	 * Merge worker's statistics if any
	 */
	pgstrom_merge_worker_statistics(gts);

	/*
	 * Extra features if any
	 */
	initStringInfo(&buf);

	/* outer-bulk-exec? */
	if (gts->outer_bulk_exec)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfo(&buf, "%souter-bulk-exec",
							 buf.len > 0 ? ", " : "");
		else
			ExplainPropertyText("Outer Bulk Exec", "enabled", es);
	}
	else if (es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyText("Outer Bulk Exec", "disabled", es);

	/* output-(row|slot)-format */
	if (es->format == EXPLAIN_FORMAT_TEXT)
		appendStringInfo(&buf, "%soutput-%s-format",
						 buf.len > 0 ? ", " : "",
						 gts->row_format ? "row" : "slot");
	else
		ExplainPropertyText("Output Format",
							gts->row_format ? "Row" : "Slot", es);

	/* NVMe-Strom support */
	if (gts->nvme_sstate ||
		(!es->analyze &&
		 gts->css.ss.ss_currentRelation &&
		 RelationWillUseNvmeStrom(gts->css.ss.ss_currentRelation, NULL)))
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfo(&buf, "%snvme-strom",
                             buf.len > 0 ? ", " : "");
		else
			ExplainPropertyText("NVMe-Strom", "enabled", es);
	}
	else if (es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyText("NVMe-Strom", "disabled", es);

	/* Dump extra features */
	if (es->format == EXPLAIN_FORMAT_TEXT && buf.len > 0)
		ExplainPropertyText("Features", buf.data, es);
	pfree(buf.data);

	/*
	 * Show source path of the GPU kernel
	 */
	if (es->verbose &&
		gts->program_id != INVALID_PROGRAM_ID &&
		pgstrom_debug_kernel_source)
	{
		const char *cuda_source = pgstrom_cuda_source_file(gts->program_id);

		ExplainPropertyText("Kernel Source", cuda_source, es);
	}
}

/*
 * pgstromInitGpuTask
 */
void
pgstromInitGpuTask(GpuTaskState *gts, GpuTask *gtask)
{
	gtask->task_kind    = gts->task_kind;
	gtask->program_id   = gts->program_id;
	gtask->gts          = gts;
	gtask->revision     = gts->revision;
	gtask->row_format   = gts->row_format;
	gtask->cpu_fallback = false;
	gtask->file_desc    = -1;
	/* fields used in server */
	gtask->gcontext		= NULL;
	gtask->gmod_cache	= NULL;
	memset(&gtask->tv_wakeup, 0, sizeof(struct timeval));
	gtask->peer_fdesc   = -1;
	gtask->dma_task_id	= 0UL;
}

/*
 * pgstromProcessGpuTask - processing handler of GpuTask
 */
int
pgstromProcessGpuTask(GpuTask *gtask,
					  CUmodule cuda_module,
					  CUstream cuda_stream)
{
	int		retval;

	/* should be called under multi-threading mode */
	Assert(IsGpuServerProcess() < 0);

	switch (gtask->task_kind)
	{
		case GpuTaskKind_GpuScan:
			retval = gpuscan_process_task(gtask, cuda_module, cuda_stream);
			break;
		case GpuTaskKind_GpuJoin:
			retval = gpujoin_process_task(gtask, cuda_module, cuda_stream);
			break;
		case GpuTaskKind_GpuPreAgg:
			retval = gpupreagg_process_task(gtask, cuda_module, cuda_stream);
			break;
		case GpuTaskKind_PL_CUDA:
			retval = plcuda_process_task(gtask, cuda_module, cuda_stream);
			break;
		default:
			elog(ERROR, "Unknown GpuTask kind: %d", gtask->task_kind);
			break;
	}
	return retval;
}

/*
 * pgstromCompleteGpuTask - completion handler of GpuTask
 */
int
pgstromCompleteGpuTask(GpuTask *gtask)
{
	int		retval;

	/* should be called under multi-threading mode */
	Assert(IsGpuServerProcess() < 0);

	switch (gtask->task_kind)
	{
		case GpuTaskKind_GpuScan:
			retval = gpuscan_complete_task(gtask);
			break;
		case GpuTaskKind_GpuJoin:
			retval = gpujoin_complete_task(gtask);
			break;
		case GpuTaskKind_GpuPreAgg:
			retval = gpupreagg_complete_task(gtask);
			break;
		case GpuTaskKind_PL_CUDA:
			retval = plcuda_complete_task(gtask);
			break;
		default:
			elog(ERROR, "Unknown GpuTask kind: %d", (int)gtask->task_kind);
			break;
	}
	return retval;
}

/*
 * pgstromReleaseGpuTask - release of GpuTask
 */
void
pgstromReleaseGpuTask(GpuTask *gtask)
{
	switch (gtask->task_kind)
	{
		case GpuTaskKind_GpuScan:
			gpuscan_release_task(gtask);
			break;
		case GpuTaskKind_GpuJoin:
			gpujoin_release_task(gtask);
			break;
		case GpuTaskKind_GpuPreAgg:
			gpupreagg_release_task(gtask);
			break;
		case GpuTaskKind_PL_CUDA:
			plcuda_release_task(gtask);
			break;
		default:
			elog(ERROR, "Unknown GpuTask kind: %d", (int)gtask->task_kind);
			break;
	}
}

/*
 * errorText - string form of the error code
 */
const char *
__errorText(int errcode, const char *__filename, int lineno)
{
	static __thread char buffer[800];
	const char	   *filename;
	const char	   *label;

	/* pick up the last component of the relative pathname */
	filename = strrchr(__filename, '/');
	filename = (!filename ? __filename : filename + 1);

	switch (errcode)
	{
		case StromError_Success:
			label = "Suceess";
			break;
		case StromError_CpuReCheck:
			label = "CPU ReCheck";
			break;
		case StromError_CudaInternal:
			label = "CUDA Internal Error";
			break;
		case StromError_OutOfMemory:
			label = "Out of memory";
			break;
		case StromError_OutOfSharedMemory:
			label = "Out of shared memory";
			break;
		case StromError_OutOfKernelArgs:
			label = "Out of kernel argument buffer";
			break;
		case StromError_InvalidValue:
			label = "Invalid Value";
			break;
		case StromError_DataStoreCorruption:
			label = "Data store corruption";
			break;
		case StromError_DataStoreNoSpace:
			label = "Data store no space";
			break;
		case StromError_DataStoreOutOfRange:
			label = "Data store out of range";
			break;
		case StromError_SanityCheckViolation:
			label = "Sanity check violation";
			break;

		/*
		 * CUDA Runtime Error - we don't want to link entire CUDA runtime
		 * for error code handling only.
		 */
#define RT_ERROR(ERRCODE, ERRNAME)										\
			case (StromError_CudaDevRunTimeBase + (ERRCODE)):			\
				label = "CUDA Runtime Error " #ERRCODE " - " #ERRNAME;	\
				break;
			RT_ERROR(1, MissingConfiguration);
			RT_ERROR(2, MemoryAllocation);
			RT_ERROR(3, InitializationError);
			RT_ERROR(4, LaunchFailure);
			RT_ERROR(5, PriorLaunchFailure);
			RT_ERROR(6, LaunchTimeout);
			RT_ERROR(7, LaunchOutOfResources);
			RT_ERROR(8, InvalidDeviceFunction);
			RT_ERROR(9, InvalidConfiguration);
			RT_ERROR(10, InvalidDevice);
			RT_ERROR(11, InvalidValue);
			RT_ERROR(12, InvalidPitchValue);
			RT_ERROR(13, InvalidSymbol);
			RT_ERROR(14, MapBufferObjectFailed);
			RT_ERROR(15, UnmapBufferObjectFailed);
			RT_ERROR(16, InvalidHostPointer);
			RT_ERROR(17, InvalidDevicePointer);
			RT_ERROR(18, InvalidTexture);
			RT_ERROR(19, InvalidTextureBinding);
			RT_ERROR(20, InvalidChannelDescriptor);
			RT_ERROR(21, InvalidMemcpyDirection);
			RT_ERROR(22, AddressOfConstant);
			RT_ERROR(23, TextureFetchFailed);
			RT_ERROR(24, TextureNotBound);
			RT_ERROR(25, SynchronizationError);
			RT_ERROR(26, InvalidFilterSetting);
			RT_ERROR(27, InvalidNormSetting);
			RT_ERROR(28, MixedDeviceExecution);
			RT_ERROR(29, CudartUnloading);
			RT_ERROR(30, Unknown);
			RT_ERROR(31, NotYetImplemented);
			RT_ERROR(32, MemoryValueTooLarge);
			RT_ERROR(33, InvalidResourceHandle);
			RT_ERROR(34, NotReady);
			RT_ERROR(35, InsufficientDriver);
			RT_ERROR(36, SetOnActiveProcess);
			RT_ERROR(37, InvalidSurface);
			RT_ERROR(38, NoDevice);
			RT_ERROR(39, ECCUncorrectable);
			RT_ERROR(40, SharedObjectSymbolNotFound);
			RT_ERROR(41, SharedObjectInitFailed);
			RT_ERROR(42, UnsupportedLimit);
			RT_ERROR(43, DuplicateVariableName);
			RT_ERROR(44, DuplicateTextureName);
			RT_ERROR(45, DuplicateSurfaceName);
			RT_ERROR(46, DevicesUnavailable);
			RT_ERROR(47, InvalidKernelImage);
			RT_ERROR(48, NoKernelImageForDevice);
			RT_ERROR(49, IncompatibleDriverContext);
			RT_ERROR(50, PeerAccessAlreadyEnabled);
			RT_ERROR(51, PeerAccessNotEnabled);
			RT_ERROR(54, DeviceAlreadyInUse);
			RT_ERROR(55, ProfilerDisabled);
			RT_ERROR(56, ProfilerNotInitialized);
			RT_ERROR(57, ProfilerAlreadyStarted);
			RT_ERROR(58, ProfilerAlreadyStopped);
			RT_ERROR(59, Assert);
			RT_ERROR(60, TooManyPeers);
			RT_ERROR(61, HostMemoryAlreadyRegistered);
			RT_ERROR(62, HostMemoryNotRegistered);
			RT_ERROR(63, OperatingSystem);
			RT_ERROR(64, PeerAccessUnsupported);
			RT_ERROR(65, LaunchMaxDepthExceeded);
			RT_ERROR(66, LaunchFileScopedTex);
			RT_ERROR(67, LaunchFileScopedSurf);
			RT_ERROR(68, SyncDepthExceeded);
			RT_ERROR(69, LaunchPendingCountExceeded);
			RT_ERROR(70, NotPermitted);
			RT_ERROR(71, NotSupported);
			RT_ERROR(72, HardwareStackError);
			RT_ERROR(73, IllegalInstruction);
			RT_ERROR(74, MisalignedAddress);
			RT_ERROR(75, InvalidAddressSpace);
			RT_ERROR(76, InvalidPc);
			RT_ERROR(77, IllegalAddress);
			RT_ERROR(78, InvalidPtx);
			RT_ERROR(79, InvalidGraphicsContext);
			RT_ERROR(127, StartupFailure);
#undef RT_ERROR

		default:
			if (errcode <= CUDA_ERROR_UNKNOWN)
			{
				const char *error_val;
				const char *error_str;

				/* Likely CUDA driver error */
				if (cuGetErrorName(errcode, &error_val) == CUDA_SUCCESS &&
					cuGetErrorString(errcode, &error_str) == CUDA_SUCCESS)
					snprintf(buffer, sizeof(buffer), "%s - %s (%s:%d)",
							 error_val, error_str,
							 filename, lineno);
				else
					snprintf(buffer, sizeof(buffer), "%d - unknown (%s:%d)",
							 errcode,
							 filename, lineno);
			}
			else if (errcode >= StromError_CudaDevRunTimeBase)
			{
				/* Or, unknown CUDA runtime error */
				snprintf(buffer, sizeof(buffer),
						 "CUDA Runtime Error %d - unknown (%s:%d)",
						 errcode - StromError_CudaDevRunTimeBase,
						 filename, lineno);
			}
			else
			{
				/* ??? Unknown PG-Strom error??? */
				snprintf(buffer, sizeof(buffer),
						 "Unexpected Error: %d (%s:%d)",
						 errcode,
						 filename, lineno);
			}
			return buffer;
	}
	snprintf(buffer, sizeof(buffer), "%s (%s:%d)", label, filename, lineno);
	return buffer;
}

/*
 * errorTextKernel - string form of the kern_errorbuf
 */
const char *
errorTextKernel(kern_errorbuf *kerror)
{
	static __thread char buffer[1024];
	const char *kernel_name;

#define KERN_ENTRY(KERNEL)						\
	case StromKernel_##KERNEL: kernel_name = #KERNEL; break

	switch (kerror->kernel)
	{
		KERN_ENTRY(HostPGStrom);
		KERN_ENTRY(CudaRuntime);
		KERN_ENTRY(NVMeStrom);
		KERN_ENTRY(gpuscan_exec_quals_block);
		KERN_ENTRY(gpuscan_exec_quals_row);
		KERN_ENTRY(gpuscan_projection_row);
		KERN_ENTRY(gpuscan_projection_slot);
		KERN_ENTRY(gpuscan_main);
		KERN_ENTRY(gpujoin_exec_outerscan);
		KERN_ENTRY(gpujoin_exec_nestloop);
		KERN_ENTRY(gpujoin_exec_hashjoin);
		KERN_ENTRY(gpujoin_outer_nestloop);
		KERN_ENTRY(gpujoin_outer_hashjoin);
		KERN_ENTRY(gpujoin_projection_row);
		KERN_ENTRY(gpujoin_projection_slot);
		KERN_ENTRY(gpujoin_count_rows_dist);
		KERN_ENTRY(gpujoin_main);
		KERN_ENTRY(gpupreagg_preparation);
		KERN_ENTRY(gpupreagg_local_reduction);
		KERN_ENTRY(gpupreagg_global_reduction);
		KERN_ENTRY(gpupreagg_nogroup_reduction);
		KERN_ENTRY(gpupreagg_final_preparation);
		KERN_ENTRY(gpupreagg_final_reduction);
		KERN_ENTRY(gpupreagg_fixup_varlena);
		KERN_ENTRY(gpupreagg_main);
		KERN_ENTRY(gpusort_projection);
		KERN_ENTRY(gpusort_bitonic_local);
		KERN_ENTRY(gpusort_bitonic_step);
		KERN_ENTRY(gpusort_bitonic_merge);
		KERN_ENTRY(gpusort_fixup_pointers);
		KERN_ENTRY(gpusort_main);
		KERN_ENTRY(plcuda_prep_kernel);
		KERN_ENTRY(plcuda_main_kernel);
		KERN_ENTRY(plcuda_post_kernel);
		default:
			kernel_name = "unknown kernel";
			break;
	}
#undef KERN_ENTRY
	snprintf(buffer, sizeof(buffer), "%s:%d %s",
			 kernel_name, kerror->lineno,
			 errorText(kerror->errcode));
	return buffer;
}

/* ------------------------------------------------------------ *
 *   Misc routines to support EXPLAIN command
 * ------------------------------------------------------------ */
/*
 * pgstromExplainOuterScan
 */
void
pgstromExplainOuterScan(GpuTaskState *gts,
						List *deparse_context,
						List *ancestors,
						ExplainState *es,
						List *outer_quals,
						Cost outer_startup_cost,
						Cost outer_total_cost,
						double outer_plan_rows,
						int outer_plan_width)
{
	Plan		   *plannode = gts->css.ss.ps.plan;
	Index			scanrelid = ((Scan *) plannode)->scanrelid;
	Instrumentation *instrument = &gts->outer_instrument;
	RangeTblEntry  *rte;
	const char	   *refname;
	const char	   *relname;
	const char	   *nspname = NULL;
	StringInfoData	str;

	/* Does this GpuTaskState has outer simple scan? */
	if (scanrelid == 0)
		return;

	/*
	 * See the logic in ExplainTargetRel()
	 */
	rte = rt_fetch(scanrelid, es->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	refname = (char *) list_nth(es->rtable_names, scanrelid - 1);
	if (!refname)
		refname = rte->eref->aliasname;
	relname = get_rel_name(rte->relid);
	if (es->verbose)
		nspname = get_namespace_name(get_rel_namespace(rte->relid));

	initStringInfo(&str);
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		if (nspname != NULL)
			appendStringInfo(&str, "%s.%s",
							 quote_identifier(nspname),
							 quote_identifier(relname));
		else if (relname)
			appendStringInfo(&str, "%s",
							 quote_identifier(relname));
		if (!relname || strcmp(refname, relname) != 0)
		{
			if (str.len > 0)
				appendStringInfoChar(&str, ' ');
			appendStringInfo(&str, "%s", refname);
		}
	}
	else
	{
		ExplainPropertyText("Outer Scan Relation", relname, es);
		if (nspname)
			ExplainPropertyText("Outer Scan Schema", nspname, es);
		ExplainPropertyText("Outer Scan Alias", refname, es);
	}

	if (es->costs)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfo(&str, "  (cost=%.2f..%.2f rows=%.0f width=%d)",
							 outer_startup_cost,
							 outer_total_cost,
							 outer_plan_rows,
							 outer_plan_width);
		else
		{
			ExplainPropertyFloat("Outer Startup Cost",
								 outer_startup_cost, 2, es);
			ExplainPropertyFloat("Outer Total Cost", outer_total_cost, 2, es);
			ExplainPropertyFloat("Outer Plan Rows", outer_plan_rows, 0, es);
			ExplainPropertyInteger("Outer Plan Width", outer_plan_width, es);
		}
	}

	/*
	 * We have to forcibly clean up the instrumentation state because we
	 * haven't done ExecutorEnd yet.  This is pretty grotty ...
	 * See the comment in ExplainNode()
	 */
	InstrEndLoop(instrument);

	if (es->analyze && instrument->nloops > 0)
	{
		double	nloops = instrument->nloops;
		double	startup_sec = 1000.0 * instrument->startup / nloops;
		double	total_sec = 1000.0 * instrument->total / nloops;
		double	rows = instrument->ntuples / nloops;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			if (es->timing)
				appendStringInfo(
					&str,
					" (actual time=%.3f..%.3f rows=%.0f loops=%.0f)",
					startup_sec, total_sec, rows, nloops);
			else
				appendStringInfo(
					&str,
					" (actual rows=%.0f loops=%.0f)",
					rows, nloops);
		}
		else
		{
			if (es->timing)
			{
				ExplainPropertyFloat("Outer Actual Startup Time",
									 startup_sec, 3, es);
				ExplainPropertyFloat("Outer Actual Total Time",
									 total_sec, 3, es);
			}
			ExplainPropertyFloat("Outer Actual Rows", rows, 0, es);
			ExplainPropertyFloat("Outer Actual Loops", nloops, 0, es);
		}
	}
	else if (es->analyze)
	{
		if (es->format == EXPLAIN_FORMAT_TEXT)
			appendStringInfoString(&str, " (never executed)");
		else
		{
			if (es->timing)
			{
				ExplainPropertyFloat("Outer Actual Startup Time", 0.0, 3, es);
				ExplainPropertyFloat("Outer Actual Total Time", 0.0, 3, es);
			}
			ExplainPropertyFloat("Outer Actual Rows", 0.0, 0, es);
			ExplainPropertyFloat("Outer Actual Loops", 0.0, 0, es);
		}
	}
	if (es->format == EXPLAIN_FORMAT_TEXT)
		ExplainPropertyText("Outer Scan", str.data, es);

	if (outer_quals != NIL)
	{
		Node   *outer_quals_expr = (Node *)make_ands_explicit(outer_quals);
		char   *outer_quals_str;

		outer_quals_str = deparse_expression(outer_quals_expr,
											 deparse_context,
											 es->verbose, false);
		ExplainPropertyText("Outer Scan Filter", outer_quals_str, es);

		if (gts->outer_instrument.nfiltered1 > 0.0)
			ExplainPropertyFloat("Rows Removed by Outer Scan Filter",
								 gts->outer_instrument.nfiltered1 /
								 gts->outer_instrument.nloops,
								 0, es);
	}
}


#if 0
void
pgstrom_explain_expression(List *expr_list, const char *qlabel,
						   PlanState *planstate, List *deparse_context,
						   List *ancestors, ExplainState *es,
						   bool force_prefix, bool convert_to_and)
{
	bool        useprefix = (force_prefix | es->verbose);
	char       *exprstr;

	/* No work if empty expression list */
	if (expr_list == NIL)
		return;

	/* Deparse the expression */
	/* List shall be replaced by explicit AND, if needed */
	exprstr = deparse_expression(convert_to_and
								 ? (Node *) make_ands_explicit(expr_list)
								 : (Node *) expr_list,
								 deparse_context,
								 useprefix,
								 false);
	/* And add to es->str */
	ExplainPropertyText(qlabel, exprstr, es);
}
#endif

#if 0
void
show_scan_qual(List *qual, const char *qlabel,
               PlanState *planstate, List *ancestors,
               ExplainState *es)
{
	bool        useprefix;
	Node	   *node;
	List       *context;
	char       *exprstr;

	useprefix = (IsA(planstate->plan, SubqueryScan) || es->verbose);

	/* No work if empty qual */
	if (qual == NIL)
		return;

	/* Convert AND list to explicit AND */
	node = (Node *) make_ands_explicit(qual);

	/* Set up deparsing context */
	context = set_deparse_context_planstate(es->deparse_cxt,
											(Node *) planstate,
											ancestors);
	/* Deparse the expression */
	exprstr = deparse_expression(node, context, useprefix, false);

	/* And add to es->str */
	ExplainPropertyText(qlabel, exprstr, es);
}
#endif
#if 0
/*
 * If it's EXPLAIN ANALYZE, show instrumentation information for a plan node
 *
 * "which" identifies which instrumentation counter to print
 */
void
show_instrumentation_count(const char *qlabel, int which,
						   PlanState *planstate, ExplainState *es)
{
	double		nfiltered;
	double		nloops;

	if (!es->analyze || !planstate->instrument)
		return;

	if (which == 2)
		nfiltered = planstate->instrument->nfiltered2;
	else
		nfiltered = planstate->instrument->nfiltered1;
	nloops = planstate->instrument->nloops;

	/* In text mode, suppress zero counts; they're not interesting enough */
	if (nfiltered > 0 || es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (nloops > 0)
			ExplainPropertyFloat(qlabel, nfiltered / nloops, 0, es);
		else
			ExplainPropertyFloat(qlabel, 0.0, 0, es);
	}
}
#endif

void
pgstrom_merge_worker_statistics(GpuTaskState *gts)
{
	Assert(ParallelMasterBackendId == InvalidBackendId);

	if (gts->worker_stat)
	{
		pgstromWorkerStatistics *worker_stat = gts->worker_stat;

		SpinLockAcquire(&worker_stat->lock);
		InstrAggNode(&gts->outer_instrument,
					 &worker_stat->worker_instrument);
		/* GpuJoin */
		gpujoin_merge_worker_statistics(gts);
		SpinLockRelease(&worker_stat->lock);
	}
}

static void
pgstrom_accum_worker_statistics(GpuTaskState *gts)
{
	Assert(ParallelMasterBackendId != InvalidBackendId);
	if (gts->worker_stat)
	{
		pgstromWorkerStatistics *worker_stat = gts->worker_stat;

		SpinLockAcquire(&worker_stat->lock);
		InstrAggNode(&worker_stat->worker_instrument,
					 &gts->outer_instrument);
		/* GpuJoin */
		gpujoin_accum_worker_statistics(gts);
		SpinLockRelease(&worker_stat->lock);
	}
}

/*
 * pgstrom_init_gputasks
 */
void
pgstrom_init_gputasks(void)
{
	/* nothing to do */
}






