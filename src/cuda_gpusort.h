/*
 * opencl_gpupreagg.h
 *
 * Preprocess of aggregate using GPU acceleration, to reduce number of
 * rows to be processed by CPU; including the Sort reduction.
 * --
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
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
#ifndef CUDA_GPUSORT_H
#define CUDA_GPUSORT_H

/*
 * GPU Accelerated Sorting
 *
 * It packs kern_parambu, status field, and kern_row_map structure
 * within a continuous memory area, to translate this chunk with
 * a single DMA call.
 *
 * +----------------+
 * | kern_parambuf  |
 * | +--------------+
 * | | length   o---------+
 * | +--------------+     | kern_resultbuf is located just after
 * | | nparams      |     | the kern_parambuf (because of DMA
 * | +--------------+     | optimization), so head address of
 * | | poffset[0]   |     | kern_gpuscan + parambuf.length
 * | | poffset[1]   |     | points kern_row_map.
 * | |    :         |     |
 * | | poffset[M-1] |     |
 * | +--------------+     |
 * | | variable     |     |
 * | | length field |     |
 * | | for Param /  |     |
 * | | Const values |     |
 * | |     :        |     |
 * +-+--------------+ <---+
 * | kern_resultbuf | <----
 * | +--------------+
 * | | nrels (=2)   |
 * | +--------------+
 * | | nrooms       |
 * | +--------------+
 * | | nitems       |
 * | +--------------+
 * | | errcode      |
 * | +--------------+
 * | | has_rechecks |
 * | +--------------+
 * | | all_visible  |
 * | +--------------+
 * | | __padding__[]|
 * | +--------------+
 * | | results[0]   | A pair of results identify the records being sorted.
 * | | results[1]   | result[even number] indicates chunk_id.
 * | +--------------+   (It is always same in a single kernel execution)
 * | | results[2]   | result[odd number] indicated item_id; that is index
 * | | results[3]   |   of a row within a sorting chunk
 * | +--------------+
 * | |     :        |
 * +-+--------------+  -----
 */
typedef struct
{
	kern_parambuf	kparams;
	/* kern_resultbuf (nrels = 2) shall be located next to the kparams */
} kern_gpusort;

#define KERN_GPUSORT_PARAMBUF(kgpusort)			(&(kgpusort)->kparams)
#define KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort)	((kgpusort)->kparams.length)
#define KERN_GPUSORT_RESULTBUF(kgpusort)			\
	((kern_resultbuf *)								\
	 ((char *)KERN_GPUSORT_PARAMBUF(kgpusort) +		\
	  KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort)))
#define KERN_GPUSORT_RESULTBUF_LENGTH(kgpusort)		\
	STROMALIGN(offsetof(kern_resultbuf,				\
		results[KERN_GPUSORT_RESULTBUF(kgpusort)->nrels * \
				KERN_GPUSORT_RESULTBUF(kgpusort)->nrooms]))
#define KERN_GPUSORT_LENGTH(kgpusort)				\
	(offsetof(kern_gpusort, kparams) +				\
	 KERN_GPUSORT_PARAMBUF_LENGTH(kgpusort) +		\
	 KERN_GPUSORT_RESULTBUF_LENGTH(kgpusort))
#define KERN_GPUSORT_DMASEND_OFFSET(kgpusort)		\
	offsetof(kern_gpusort, kparams)
#define KERN_GPUSORT_DMASEND_LENGTH(kgpusort)		\
	(KERN_GPUSORT_LENGTH(kgpusort) -				\
	 offsetof(kern_gpusort, kparams))
#define KERN_GPUSORT_DMARECV_OFFSET(kgpusort)		\
	((uintptr_t)KERN_GPUSORT_RESULTBUF(kgpusort) -	\
	 (uintptr_t)(kgpusort))
#define KERN_GPUSORT_DMARECV_LENGTH(kgpusort)		\
	KERN_GPUSORT_RESULTBUF_LENGTH(kgpusort)

#ifdef __CUDACC__
/*
 * Sorting key comparison function - to be generated by PG-Strom
 * on the fly.
 */
STATIC_FUNCTION(cl_int)
gpusort_keycomp(kern_context *kcxt,
				kern_data_store *kds,
				kern_data_store *ktoast,
				size_t x_index,
				size_t y_index);
/*
 * Projection of sorting key - to be generated by PG-Strom on the fly.
 */
STATIC_FUNCTION(void)
gpusort_projection(kern_context *kcxt,
				   Datum *ts_values,
				   cl_char *ts_isnull,
				   kern_data_store *ktoast,
				   HeapTupleHeaderData *htup);
/*
 * Fixup special internal variables (numeric, at this moment)
 */
STATIC_FUNCTION(void)
gpusort_fixup_variables(kern_context *kcxt,
						Datum *ts_values,
						cl_char *ts_isnull,
						kern_data_store *ktoast,
						HeapTupleHeaderData *htup);

/*
 * gpusort_preparation - fill up krowmap->rindex array and setup
 * kds (tupslot format) according to the ktoast (row-flat format)
 */
KERNEL_FUNCTION(void)
gpusort_preparation(kern_gpusort *kgpusort,		/* in */
					kern_data_store *kds,		/* out */
					kern_data_store *ktoast,	/* in */
					cl_int chunk_id)
{
	kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_context	kcxt;
	size_t			nitems = ktoast->nitems;
	size_t			index;

	/* sanity checks */
	assert(kresults->nrels == 2);
	assert(kresults->nitems == nitems);
	assert(ktoast->format == KDS_FORMAT_ROW);
	assert(kds->format == KDS_FORMAT_SLOT);

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_preparation, kparams);
	if (kds->nrooms < nitems)
	{
		STROM_SET_ERROR(&kcxt.e, StromError_DataStoreNoSpace);
		goto out;
	}

	/* kds also has same nitems */
	if (get_global_id() == 0)
		kds->nitems = nitems;

	/* put initial value of row-index */
	for (index = get_global_id();
		 index < nitems;
		 index += get_global_size())
	{
		kresults->results[2 * index] = chunk_id;
		kresults->results[2 * index + 1] = index;
	}

	/* projection of kds */
	if (get_global_id() < nitems)
	{
		HeapTupleHeaderData *htup;
		Datum	   *ts_values;
		cl_char	   *ts_isnull;

		htup = kern_get_tuple_row(ktoast, get_global_id());
		if (!htup)
		{
			STROM_SET_ERROR(&kcxt.e, StromError_DataStoreCorruption);
			goto out;
		}
		ts_values = KERN_DATA_STORE_VALUES(kds, get_global_id());
		ts_isnull = KERN_DATA_STORE_ISNULL(kds, get_global_id());
		gpusort_projection(&kcxt,
						   ts_values,
						   ts_isnull,
						   ktoast, htup);
	}
out:
	kern_writeback_error_status(&kresults->kerror, kcxt.e);
}

/*
 * gpusort_bitonic_local
 *
 * It tries to apply each steps of bitonic-sorting until its unitsize
 * reaches the workgroup-size (that is expected to power of 2).
 */
KERNEL_FUNCTION_MAXTHREADS(void)
gpusort_bitonic_local(kern_gpusort *kgpusort,
					  kern_data_store *kds,
					  kern_data_store *ktoast)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	kern_context	kcxt;
	cl_int		   *localIdx = SHARED_WORKMEM(cl_int);
	cl_uint			nitems = kds->nitems;
	size_t			localID = get_local_id();
	size_t			globalID = get_global_id();
	size_t			localSize = get_local_size();
	size_t			prtID = globalID / localSize;	/* partition ID */
	size_t			prtSize = localSize * 2;		/* partition Size */
	size_t			prtPos = prtID * prtSize;		/* partition Position */
	size_t			localEntry;
	size_t			blockSize;
	size_t			unitSize;
	size_t			i;

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_bitonic_local, kparams);

	/* Load index to localIdx[] */
	localEntry = (prtPos + prtSize < nitems) ? prtSize : (nitems - prtPos);
	for (i = localID; i < localEntry; i += localSize)
		localIdx[i] = kresults->results[2 * (prtPos + i) + 1];
	__syncthreads();

	/* bitonic sorting */
	for (blockSize = 2; blockSize <= prtSize; blockSize *= 2)
	{
		for (unitSize = blockSize; unitSize >= 2; unitSize /= 2)
        {
			size_t	unitMask		= unitSize - 1;
			size_t	halfUnitSize	= unitSize / 2;
			bool	reversing  = (unitSize == blockSize ? true : false);
			size_t	idx0 = ((localID / halfUnitSize) * unitSize
							+ localID % halfUnitSize);
            size_t	idx1 = ((reversing == true)
							? ((idx0 & ~unitMask) | (~idx0 & unitMask))
							: (halfUnitSize + idx0));

            if(idx1 < localEntry)
			{
				cl_int	pos0 = localIdx[idx0];
				cl_int	pos1 = localIdx[idx1];

				if (gpusort_keycomp(&kcxt, kds, ktoast, pos0, pos1) > 0)
				{
					/* swap them */
					localIdx[idx0] = pos1;
					localIdx[idx1] = pos0;
				}
			}
			__syncthreads();
		}
	}
	/* write back local sorted result */
	for (i=localID; i < localEntry; i+=localSize)
		kresults->results[2 * (prtPos + i) + 1] = localIdx[i];
	__syncthreads();

	/* any error during run-time? */
	kern_writeback_error_status(&kresults->kerror, kcxt.e);
}

/*
 * gpusort_bitonic_step
 *
 * It tries to apply individual steps of bitonic-sorting for each step,
 * but does not have restriction of workgroup size. The host code has to
 * control synchronization of each step not to overrun.
 */
KERNEL_FUNCTION_MAXTHREADS(void)
gpusort_bitonic_step(kern_gpusort *kgpusort,
					 kern_data_store *kds,
					 kern_data_store *ktoast,
					 cl_int bitonic_unitsz)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	kern_context	kcxt;
	cl_bool			reversing = (bitonic_unitsz < 0 ? true : false);
	size_t			unitsz = (bitonic_unitsz < 0
							  ? -bitonic_unitsz
							  : bitonic_unitsz);
	cl_uint			nitems = kds->nitems;
	size_t			globalID = get_global_id();
	size_t			halfUnitSize = unitsz / 2;
	size_t			unitMask = unitsz - 1;
	cl_int			idx0, idx1;
	cl_int			pos0, pos1;

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_bitonic_step, kparams);

	idx0 = (globalID / halfUnitSize) * unitsz + globalID % halfUnitSize;
	idx1 = (reversing
			? ((idx0 & ~unitMask) | (~idx0 & unitMask))
			: (idx0 + halfUnitSize));
	if (idx1 >= nitems)
		goto out;

	pos0 = kresults->results[2 * idx0 + 1];
	pos1 = kresults->results[2 * idx1 + 1];
	if (gpusort_keycomp(&kcxt, kds, ktoast, pos0, pos1) > 0)
	{
		/* swap them */
		kresults->results[2 * idx0 + 1] = pos1;
		kresults->results[2 * idx1 + 1] = pos0;
	}
out:
	kern_writeback_error_status(&kresults->kerror, kcxt.e);
}

/*
 * gpusort_bitonic_merge
 *
 * It handles the merging step of bitonic-sorting if unitsize becomes less
 * than or equal to the workgroup size.
 */
KERNEL_FUNCTION_MAXTHREADS(void)
gpusort_bitonic_merge(kern_gpusort *kgpusort,
					  kern_data_store *kds,
					  kern_data_store *ktoast)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	kern_context	kcxt;
	cl_int		   *localIdx = SHARED_WORKMEM(cl_int);
	cl_uint			nitems = kds->nitems;
    size_t			localID = get_local_id();
    size_t			globalID = get_global_id();
    size_t			localSize = get_local_size();
	size_t			prtID = globalID / localSize;	/* partition ID */
	size_t			prtSize = 2 * localSize;		/* partition Size */
	size_t			prtPos = prtID * prtSize;		/* partition Position */
	size_t			localEntry;
	size_t			blockSize = prtSize;
	size_t			unitSize = prtSize;
	size_t			i;

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_bitonic_merge, kparams);

	/* Load index to localIdx[] */
	localEntry = (prtPos+prtSize < nitems) ? prtSize : (nitems-prtPos);
	for (i = localID; i < localEntry; i += localSize)
		localIdx[i] = kresults->results[2 * (prtPos + i) + 1];
	__syncthreads();

	/* merge two sorted blocks */
	for (unitSize = blockSize; unitSize >= 2; unitSize /= 2)
	{
		size_t	halfUnitSize = unitSize / 2;
		size_t	idx0, idx1;

		idx0 = localID / halfUnitSize * unitSize + localID % halfUnitSize;
		idx1 = halfUnitSize + idx0;

        if (idx1 < localEntry)
		{
			size_t	pos0 = localIdx[idx0];
			size_t	pos1 = localIdx[idx1];

			if (gpusort_keycomp(&kcxt, kds, ktoast, pos0, pos1) > 0)
			{
				/* swap them */
				localIdx[idx0] = pos1;
                localIdx[idx1] = pos0;
			}
		}
		__syncthreads();
	}
	/* Save index to kresults[] */
	for (i = localID; i < localEntry; i += localSize)
		kresults->results[2 * (prtPos + i) + 1] = localIdx[i];
	__syncthreads();

	kern_writeback_error_status(&kresults->kerror, kcxt.e);
}

KERNEL_FUNCTION(void)
gpusort_fixup_datastore(kern_gpusort *kgpusort,
						kern_data_store *kds,
						kern_data_store *ktoast)
{
	kern_parambuf  *kparams = KERN_GPUSORT_PARAMBUF(kgpusort);
	kern_resultbuf *kresults = KERN_GPUSORT_RESULTBUF(kgpusort);
	kern_context	kcxt;

	INIT_KERNEL_CONTEXT(&kcxt, gpusort_fixup_datastore, kparams);

	if (get_global_id() < kds->nitems)
	{
		HeapTupleHeaderData *htup;
		Datum	   *ts_values;
		cl_char	   *ts_isnull;

		htup = kern_get_tuple_row(ktoast, get_global_id());
		assert(htup != NULL);

		ts_values = KERN_DATA_STORE_VALUES(kds, get_global_id());
		ts_isnull = KERN_DATA_STORE_ISNULL(kds, get_global_id());
		gpusort_fixup_variables(&kcxt,
								ts_values,
								ts_isnull,
								ktoast, htup);
	}
	kern_writeback_error_status(&kresults->kerror, kcxt.e);
}
#endif	/* __CUDACC__ */
#endif	/* CUDA_GPUSORT_H */
