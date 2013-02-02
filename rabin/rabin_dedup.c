/*
 * The rabin polynomial computation is derived from:
 * http://code.google.com/p/rabin-fingerprint-c/
 * 
 * originally created by Joel Lawrence Tucci on 09-March-2011.
 * 
 * Rabin polynomial portions Copyright (c) 2011 Joel Lawrence Tucci
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * Neither the name of the project's author nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2012 Moinak Ghosh. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * moinakg@belenix.org, http://moinakg.wordpress.com/
 * 
 */

#ifndef __STDC_FORMAT_MACROS
#define	__STDC_FORMAT_MACROS	1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <allocator.h>
#include <utils.h>
#include <pthread.h>
#include <heapq.h>
#include <xxhash.h>

#include "rabin_dedup.h"
#if defined(__USE_SSE_INTRIN__) && defined(__SSE4_1__) && RAB_POLYNOMIAL_WIN_SIZE == 16
#	include <smmintrin.h>
#	define	SSE_MODE		1
#endif

#if defined(_OPENMP)
#include <omp.h>
#endif

#define	DELTA_EXTRA2_PCT(x) ((x) >> 1)
#define	DELTA_EXTRA_PCT(x) (((x) >> 1) + ((x) >> 3))
#define	DELTA_NORMAL_PCT(x) (((x) >> 1) + ((x) >> 2) + ((x) >> 3))

extern int lzma_init(void **data, int *level, int nthreads, int64_t chunksize,
		     int file_version, compress_op_t op);
extern int lzma_compress(void *src, uint64_t srclen, void *dst,
	uint64_t *destlen, int level, uchar_t chdr, void *data);
extern int lzma_decompress(void *src, uint64_t srclen, void *dst,
	uint64_t *dstlen, int level, uchar_t chdr, void *data);
extern int lzma_deinit(void **data);
extern int bsdiff(u_char *oldbuf, bsize_t oldsize, u_char *newbuf, bsize_t newsize,
       u_char *diff, u_char *scratch, bsize_t scratchsize);
extern bsize_t get_bsdiff_sz(u_char *pbuf);
extern int bspatch(u_char *pbuf, u_char *oldbuf, bsize_t oldsize, u_char *newbuf,
	bsize_t *_newsize);

static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t ir[256], out[256];
static int inited = 0;

static uint32_t
dedupe_min_blksz(int rab_blk_sz)
{
	uint32_t min_blk;

	min_blk = (1 << (rab_blk_sz + RAB_BLK_MIN_BITS)) - 1024;
	return (min_blk);
}

uint32_t
dedupe_buf_extra(uint64_t chunksize, int rab_blk_sz, const char *algo, int delta_flag)
{
	if (rab_blk_sz < 1 || rab_blk_sz > 5)
		rab_blk_sz = RAB_BLK_DEFAULT;

	return ((chunksize / dedupe_min_blksz(rab_blk_sz)) * sizeof (uint32_t));
}

/*
 * Initialize the algorithm with the default params.
 */
dedupe_context_t *
create_dedupe_context(uint64_t chunksize, uint64_t real_chunksize, int rab_blk_sz,
    const char *algo, const algo_props_t *props, int delta_flag, int fixed_flag,
    int file_version, compress_op_t op) {
	dedupe_context_t *ctx;
	uint32_t i;

	if (rab_blk_sz < 1 || rab_blk_sz > 5)
		rab_blk_sz = RAB_BLK_DEFAULT;

	if (fixed_flag) {
		delta_flag = 0;
		inited = 1;
	}

	/*
	 * Pre-compute a table of irreducible polynomial evaluations for each
	 * possible byte value.
	 */
	pthread_mutex_lock(&init_lock);
	if (!inited) {
		int term, pow, j;
		uint64_t val, poly_pow;

		poly_pow = 1;
		for (j = 0; j < RAB_POLYNOMIAL_WIN_SIZE; j++) {
			poly_pow = (poly_pow * RAB_POLYNOMIAL_CONST) & POLY_MASK;
		}

		for (j = 0; j < 256; j++) {
			term = 1;
			pow = 1;
			val = 1;
			out[j] = (j * poly_pow) & POLY_MASK;
			for (i=0; i<RAB_POLYNOMIAL_WIN_SIZE; i++) {
				if (term & FP_POLY) {
					val += ((pow * j) & POLY_MASK);
				}
				pow = (pow * RAB_POLYNOMIAL_CONST) & POLY_MASK;
				term <<= 1;
			}
			ir[j] = val;
		}
		inited = 1;
	}
	pthread_mutex_unlock(&init_lock);

	/*
	 * Rabin window size must be power of 2 for optimization.
	 */
	if (!ISP2(RAB_POLYNOMIAL_WIN_SIZE)) {
		fprintf(stderr, "Rabin window size must be a power of 2 in range 4 <= x <= 64\n");
		return (NULL);
	}

	if (chunksize < RAB_MIN_CHUNK_SIZE) {
		fprintf(stderr, "Minimum chunk size for Dedup must be %" PRIu64 " bytes\n",
		    RAB_MIN_CHUNK_SIZE);
		return (NULL);
	}

	/*
	 * For LZMA with chunksize <= LZMA Window size and/or Delta enabled we
	 * use 4K minimum Rabin block size. For everything else it is 2K based
	 * on experimentation.
	 */
	ctx = (dedupe_context_t *)slab_alloc(NULL, sizeof (dedupe_context_t));
	ctx->rabin_poly_max_block_size = RAB_POLYNOMIAL_MAX_BLOCK_SIZE;

	ctx->current_window_data = NULL;
	ctx->fixed_flag = fixed_flag;
	ctx->rabin_break_patt = 0;
	ctx->rabin_poly_avg_block_size = RAB_BLK_AVG_SZ(rab_blk_sz);
	ctx->rabin_avg_block_mask = RAB_BLK_MASK;
	ctx->rabin_poly_min_block_size = dedupe_min_blksz(rab_blk_sz);
	ctx->delta_flag = 0;
	ctx->deltac_min_distance = props->deltac_min_distance;

	/*
	 * Scale down similarity percentage based on avg block size unless user specified
	 * argument '-EE' in which case fixed 40% match is used for Delta compression.
	 */
	if (delta_flag == DELTA_NORMAL) {
		if (ctx->rabin_poly_avg_block_size < (1 << 14)) {
			ctx->delta_flag = 1;
		} else if (ctx->rabin_poly_avg_block_size < (1 << 16)) {
			ctx->delta_flag = 2;
		} else {
			ctx->delta_flag = 3;
		}
	} else if (delta_flag == DELTA_EXTRA) {
		ctx->delta_flag = 2;
	}

	if (!fixed_flag)
		ctx->blknum = chunksize / ctx->rabin_poly_min_block_size;
	else
		ctx->blknum = chunksize / ctx->rabin_poly_avg_block_size;

	if (chunksize % ctx->rabin_poly_min_block_size)
		++(ctx->blknum);

	if (ctx->blknum > RABIN_MAX_BLOCKS) {
		fprintf(stderr, "Chunk size too large for dedup.\n");
		destroy_dedupe_context(ctx);
		return (NULL);
	}
#ifndef SSE_MODE
	ctx->current_window_data = (uchar_t *)slab_alloc(NULL, RAB_POLYNOMIAL_WIN_SIZE);
#else
	ctx->current_window_data = (uchar_t *)1;
#endif
	ctx->blocks = NULL;
	if (real_chunksize > 0) {
		ctx->blocks = (rabin_blockentry_t **)slab_calloc(NULL,
			ctx->blknum, sizeof (rabin_blockentry_t *));
	}
	if(ctx == NULL || ctx->current_window_data == NULL ||
	    (ctx->blocks == NULL && real_chunksize > 0)) {
		fprintf(stderr,
		    "Could not allocate rabin polynomial context, out of memory\n");
		destroy_dedupe_context(ctx);
		return (NULL);
	}

	ctx->lzma_data = NULL;
	ctx->level = 14;
	if (real_chunksize > 0) {
		lzma_init(&(ctx->lzma_data), &(ctx->level), 1, chunksize, file_version, op);

		// The lzma_data member is not needed during decompression
		if (!(ctx->lzma_data) && op == COMPRESS) {
			fprintf(stderr,
			    "Could not initialize LZMA data for dedupe index, out of memory\n");
			destroy_dedupe_context(ctx);
			return (NULL);
		}
	}

	slab_cache_add(sizeof (rabin_blockentry_t));
	ctx->real_chunksize = real_chunksize;
	reset_dedupe_context(ctx);
	return (ctx);
}

void
reset_dedupe_context(dedupe_context_t *ctx)
{
#ifndef	SSE_MODE
	memset(ctx->current_window_data, 0, RAB_POLYNOMIAL_WIN_SIZE);
#endif
	ctx->valid = 0;
}

void
destroy_dedupe_context(dedupe_context_t *ctx)
{
	if (ctx) {
		uint32_t i;
#ifndef SSE_MODE
		if (ctx->current_window_data) slab_free(NULL, ctx->current_window_data);
#endif
		if (ctx->blocks) {
			for (i=0; i<ctx->blknum && ctx->blocks[i] != NULL; i++) {
				slab_free(NULL, ctx->blocks[i]);
			}
			slab_free(NULL, ctx->blocks);
		}
		if (ctx->lzma_data) lzma_deinit(&(ctx->lzma_data));
		slab_free(NULL, ctx);
	}
}

/**
 * Perform Deduplication.
 * Both Semi-Rabin fingerprinting based and Fixed Block Deduplication are supported.
 * A 16-byte window is used for the rolling checksum and dedup blocks can vary in size
 * from 4K-128K.
 */
uint32_t
dedupe_compress(dedupe_context_t *ctx, uchar_t *buf, uint64_t *size, uint64_t offset,
		uint64_t *rabin_pos, int mt)
{
	uint64_t i, last_offset, j, ary_sz;
	uint32_t blknum, window_pos;
	uchar_t *buf1 = (uchar_t *)buf;
	uint32_t length;
	uint64_t cur_roll_checksum, cur_pos_checksum;
	uint32_t *ctx_heap;
	rabin_blockentry_t **htab;
	heap_t heap;
	DEBUG_STAT_EN(uint32_t max_count);
	DEBUG_STAT_EN(max_count = 0);
	DEBUG_STAT_EN(double strt, en_1, en);

	length = offset;
	last_offset = 0;
	blknum = 0;
	window_pos = 0;
	ctx->valid = 0;
	cur_roll_checksum = 0;
	if (*size < ctx->rabin_poly_avg_block_size) return (0);
	DEBUG_STAT_EN(strt = get_wtime_millis());

	if (ctx->fixed_flag) {
		blknum = *size / ctx->rabin_poly_avg_block_size;
		j = *size % ctx->rabin_poly_avg_block_size;
		if (j)
			++blknum;
		else
			j = ctx->rabin_poly_avg_block_size;

		last_offset = 0;
		length = ctx->rabin_poly_avg_block_size;
		for (i=0; i<blknum; i++) {
			if (i == blknum-1) {
				length = j;
			}
			if (ctx->blocks[i] == 0) {
				ctx->blocks[i] = (rabin_blockentry_t *)slab_alloc(NULL,
				    sizeof (rabin_blockentry_t));
			}
			ctx->blocks[i]->offset = last_offset;
			ctx->blocks[i]->index = i; // Need to store for sorting
			ctx->blocks[i]->length = length;
			ctx->blocks[i]->similar = 0;
			ctx->blocks[i]->hash = XXH32(buf1+last_offset, length, 0);
			ctx->blocks[i]->similarity_hash = ctx->blocks[i]->hash;
			last_offset += length;
		}
		goto process_blocks;
	}

	if (rabin_pos == NULL) {
		/*
		 * Initialize arrays for sketch computation. We re-use memory allocated
		 * for the compressed chunk temporarily.
		 */
		ary_sz = ctx->rabin_poly_max_block_size;
		ctx_heap = (uint32_t *)(ctx->cbuf + ctx->real_chunksize - ary_sz);
	}
#ifndef SSE_MODE
	memset(ctx->current_window_data, 0, RAB_POLYNOMIAL_WIN_SIZE);
#else
	__m128i cur_sse_byte = _mm_setzero_si128();
	__m128i window = _mm_setzero_si128();
#endif
	j = *size - RAB_POLYNOMIAL_WIN_SIZE;

	/* 
	 * If rabin_pos is non-zero then we are being asked to scan for the last rabin boundary
	 * in the chunk. We start scanning at chunk end - max rabin block size. We avoid doing
	 * a full chunk scan.
	 * 
	 * !!!NOTE!!!: Code duplication below for performance.
	 */
	if (rabin_pos) {
		offset = *size - ctx->rabin_poly_max_block_size;
		length = 0;
		for (i=offset; i<j; i++) {
			int cur_byte = buf1[i];
#ifdef	SSE_MODE
			uint32_t pushed_out = _mm_extract_epi32(window, 3);
			pushed_out >>= 24;
			asm ("movd %[cur_byte], %[cur_sse_byte]"
			     : [cur_sse_byte] "=x" (cur_sse_byte)
			     : [cur_byte] "r" (cur_byte)
			);
			window = _mm_slli_si128(window, 1);
			window = _mm_or_si128(window, cur_sse_byte);
#else
			uint32_t pushed_out = ctx->current_window_data[window_pos];
			ctx->current_window_data[window_pos] = cur_byte;
#endif

			cur_roll_checksum = (cur_roll_checksum * RAB_POLYNOMIAL_CONST) & POLY_MASK;
			cur_roll_checksum += cur_byte;
			cur_roll_checksum -= out[pushed_out];

#ifndef	SSE_MODE
			window_pos = (window_pos + 1) & (RAB_POLYNOMIAL_WIN_SIZE-1);
#endif
			++length;
			if (length < ctx->rabin_poly_min_block_size) continue;

			// If we hit our special value update block offset
			cur_pos_checksum = cur_roll_checksum ^ ir[pushed_out];
			if ((cur_pos_checksum & ctx->rabin_avg_block_mask) == ctx->rabin_break_patt) {
				last_offset = i;
				length = 0;
			}
		}

		if (last_offset < *size) {
			*rabin_pos = last_offset;
		}
		return (0);
	}

	/*
	 * Start our sliding window at a fixed number of bytes before the min window size.
	 * It is pointless to slide the window over the whole length of the chunk.
	 */
	offset = ctx->rabin_poly_min_block_size - RAB_WINDOW_SLIDE_OFFSET;
	length = offset;
	for (i=offset; i<j; i++) {
		uint64_t pc[4];
		uint32_t cur_byte = buf1[i];

#ifdef	SSE_MODE
		/*
		 * A 16-byte XMM register is used as a sliding window if our window size is 16 bytes
		 * and at least SSE 4.1 is enabled. Avoids memory access for the sliding window.
		 */
		uint32_t pushed_out = _mm_extract_epi32(window, 3);
		pushed_out >>= 24;

		/*
		 * No intrinsic available for this.
		 */
		asm ("movd %[cur_byte], %[cur_sse_byte]"
		     : [cur_sse_byte] "=x" (cur_sse_byte)
		     : [cur_byte] "r" (cur_byte)
		);
		window = _mm_slli_si128(window, 1);
		window = _mm_or_si128(window, cur_sse_byte);
#else
		uint32_t pushed_out = ctx->current_window_data[window_pos];
		ctx->current_window_data[window_pos] = cur_byte;
#endif

		cur_roll_checksum = (cur_roll_checksum * RAB_POLYNOMIAL_CONST) & POLY_MASK;
		cur_roll_checksum += cur_byte;
		cur_roll_checksum -= out[pushed_out];

#ifndef	SSE_MODE
		/*
		 * Window pos has to rotate from 0 .. RAB_POLYNOMIAL_WIN_SIZE-1
		 * We avoid a branch here by masking. This requires RAB_POLYNOMIAL_WIN_SIZE
		 * to be power of 2
		 */
		window_pos = (window_pos + 1) & (RAB_POLYNOMIAL_WIN_SIZE-1);
#endif
		++length;
		if (length < ctx->rabin_poly_min_block_size) continue;

		// If we hit our special value or reached the max block size update block offset
		cur_pos_checksum = cur_roll_checksum ^ ir[pushed_out];
		if ((cur_pos_checksum & ctx->rabin_avg_block_mask) == ctx->rabin_break_patt ||
		    length >= ctx->rabin_poly_max_block_size) {
			if (ctx->blocks[blknum] == 0)
				ctx->blocks[blknum] = (rabin_blockentry_t *)slab_alloc(NULL,
				    sizeof (rabin_blockentry_t));
			ctx->blocks[blknum]->offset = last_offset;
			ctx->blocks[blknum]->index = blknum; // Need to store for sorting
			ctx->blocks[blknum]->length = length;
			DEBUG_STAT_EN(if (length >= ctx->rabin_poly_max_block_size) ++max_count);

			/*
			 * Reset the heap structure and find the K min values if Delta Compression
			 * is enabled. We use a min heap mechanism taken from the heap based priority
			 * queue implementation in Python.
			 * Here K = similarity extent = 87% or 62% or 50%.
			 * 
			 * Once block contents are arranged in a min heap we compute the K min values
			 * sketch by hashing over the heap till K%. We interpret the raw bytes as a
			 * sequence of 64-bit integers.
			 * This is called minhashing and is used widely, for example in various
			 * search engines to detect similar documents.
			 */
			if (ctx->delta_flag) {
				memcpy(ctx_heap, buf1+last_offset, length);
				length /= 8;
				pc[1] = DELTA_NORMAL_PCT(length);
				pc[2] = DELTA_EXTRA_PCT(length);
				pc[3] = DELTA_EXTRA2_PCT(length);

				reset_heap(&heap, pc[ctx->delta_flag]);
				ksmallest((int64_t *)ctx_heap, length, &heap);

				ctx->blocks[blknum]->similarity_hash =
					XXH32((const uchar_t *)ctx_heap,  pc[ctx->delta_flag]*8, 0);
			}
			++blknum;
			last_offset = i+1;
			length = 0;
			if (*size - last_offset <= ctx->rabin_poly_min_block_size) break;
			length = ctx->rabin_poly_min_block_size - RAB_WINDOW_SLIDE_OFFSET;
			i = i + length;
		}
	}

	// Insert the last left-over trailing bytes, if any, into a block.
	if (last_offset < *size) {
		if (ctx->blocks[blknum] == 0)
			ctx->blocks[blknum] = (rabin_blockentry_t *)slab_alloc(NULL,
				sizeof (rabin_blockentry_t));
		ctx->blocks[blknum]->offset = last_offset;
		ctx->blocks[blknum]->index = blknum;
		length = *size - last_offset;
		ctx->blocks[blknum]->length = length;

		if (ctx->delta_flag) {
			uint64_t cur_sketch;
			uint64_t pc[3];

			if (length > ctx->rabin_poly_min_block_size) {
				memcpy(ctx_heap, buf1+last_offset, length);
				length /= 8;
				pc[1] = DELTA_NORMAL_PCT(length);
				pc[2] = DELTA_EXTRA_PCT(length);
				pc[3] = DELTA_EXTRA2_PCT(length);

				reset_heap(&heap, pc[ctx->delta_flag]);
				ksmallest((int64_t *)ctx_heap, length, &heap);
				cur_sketch =
				    XXH32((const uchar_t *)ctx_heap,  pc[ctx->delta_flag]*8, 0);
			} else {
				cur_sketch =
				    XXH32((const uchar_t *)(buf1+last_offset), length, 0);
			}
			ctx->blocks[blknum]->similarity_hash = cur_sketch;
		}
		++blknum;
		last_offset = *size;
	}

process_blocks:
	// If we found at least a few chunks, perform dedup.
	DEBUG_STAT_EN(en_1 = get_wtime_millis());
	DEBUG_STAT_EN(fprintf(stderr, "Original size: %" PRId64 ", blknum: %u\n", *size, blknum));
	DEBUG_STAT_EN(fprintf(stderr, "Number of maxlen blocks: %u\n", max_count));
	if (blknum > 2) {
		uint64_t pos, matchlen, pos1;
		int valid = 1;
		uint32_t *dedupe_index;
		uint64_t dedupe_index_sz;
		rabin_blockentry_t *be;
		DEBUG_STAT_EN(uint32_t delta_calls, delta_fails, merge_count, hash_collisions);
		DEBUG_STAT_EN(delta_calls = 0);
		DEBUG_STAT_EN(delta_fails = 0);
		DEBUG_STAT_EN(hash_collisions = 0);

		ary_sz = (blknum << 1) * sizeof (rabin_blockentry_t *);
		htab = (rabin_blockentry_t **)(ctx->cbuf + ctx->real_chunksize - ary_sz);
		memset(htab, 0, ary_sz);

		/*
		 * Compute hash signature for each block. We do this in a separate loop to 
		 * have a fast linear scan through the buffer.
		 */
		if (ctx->delta_flag) {
#if defined(_OPENMP)
#	pragma omp parallel for if (mt)
#endif
			for (i=0; i<blknum; i++) {
				ctx->blocks[i]->hash = XXH32(buf1+ctx->blocks[i]->offset,
								ctx->blocks[i]->length, 0);
			}
		} else {
#if defined(_OPENMP)
#	pragma omp parallel for if (mt)
#endif
			for (i=0; i<blknum; i++) {
				ctx->blocks[i]->hash = XXH32(buf1+ctx->blocks[i]->offset,
								ctx->blocks[i]->length, 0);
				ctx->blocks[i]->similarity_hash = ctx->blocks[i]->hash;
			}
		}

		/*
		 * Perform hash-matching of blocks and use a bucket-chained hashtable to match
		 * for duplicates and similar blocks. Unique blocks are inserted and duplicates
		 * and similar ones are marked in the block array.
		 *
		 * Hashtable memory is not allocated. We just use available space in the
		 * target buffer.
		 */
		matchlen = 0;
		for (i=0; i<blknum; i++) {
			uint64_t ck;

			/*
			 * Bias hash with length for fewer collisions. If Delta Compression is
			 * not enabled then value of similarity_hash == hash.
			 */
			ck = ctx->blocks[i]->similarity_hash;
			ck ^= (ck / ctx->blocks[i]->length);
			j = ck % (blknum << 1);

			if (htab[j] == 0) {
				/*
				 * Hash bucket empty. So add block into table.
				 */
				htab[j] = ctx->blocks[i];
				ctx->blocks[i]->other = 0;
				ctx->blocks[i]->next = 0;
				ctx->blocks[i]->similar = 0;
			} else {
				be = htab[j];
				length = 0;

				/*
				 * Look for exact duplicates. Same cksum, length and memcmp()
				 */
				while (1) {
					if (be->hash == ctx->blocks[i]->hash &&
					    be->length == ctx->blocks[i]->length &&
					    memcmp(buf1 + be->offset, buf1 + ctx->blocks[i]->offset,
					    be->length) == 0) {
						ctx->blocks[i]->similar = SIMILAR_EXACT;
						ctx->blocks[i]->other = be;
						be->similar = SIMILAR_REF;
						matchlen += be->length;
						length = 1;
						break;
					}
					if (be->next)
						be = be->next;
					else
						break;
				}

				if (ctx->delta_flag && !length) {
					/*
					 * Look for similar blocks.
					 */
					be = htab[j];
					while (1) {
						if (be->similarity_hash == ctx->blocks[i]->similarity_hash &&
						    be->length == ctx->blocks[i]->length) {
							uint64_t off_diff;
							if (be->offset > ctx->blocks[i]->offset)
								off_diff = be->offset - ctx->blocks[i]->offset;
							else
								off_diff = ctx->blocks[i]->offset - be->offset;

							if (off_diff > ctx->deltac_min_distance) {
								ctx->blocks[i]->similar = SIMILAR_PARTIAL;
								ctx->blocks[i]->other = be;
								be->similar = SIMILAR_REF;
								matchlen += (be->length>>1);
								length = 1;
								break;
							}
						}
						if (be->next)
							be = be->next;
						else
							break;
					}
				}
				/*
				 * No duplicate in table for this block. So add it to
				 * the bucket chain.
				 */
				if (!length) {
					ctx->blocks[i]->other = 0;
					ctx->blocks[i]->next = 0;
					ctx->blocks[i]->similar = 0;
					be->next = ctx->blocks[i];
					DEBUG_STAT_EN(++hash_collisions);
				}
			}
		}
		DEBUG_STAT_EN(fprintf(stderr, "Total Hashtable bucket collisions: %u\n", hash_collisions));

		dedupe_index_sz = (uint64_t)blknum * RABIN_ENTRY_SIZE;
		if (matchlen < dedupe_index_sz) {
			DEBUG_STAT_EN(en = get_wtime_millis());
			DEBUG_STAT_EN(fprintf(stderr, "Chunking speed %.3f MB/s, Overall Dedupe speed %.3f MB/s\n",
					      get_mb_s(*size, strt, en_1), get_mb_s(*size, strt, en)));
			DEBUG_STAT_EN(fprintf(stderr, "No Dedupe possible.\n"));
			ctx->valid = 0;
			return (0);
		}

		dedupe_index = (uint32_t *)(ctx->cbuf + RABIN_HDR_SIZE);
		pos = 0;
		DEBUG_STAT_EN(merge_count = 0);

		/*
		 * Merge runs of unique blocks into a single block entry to reduce
		 * dedupe index size.
		 */
		for (i=0; i<blknum;) {
			dedupe_index[pos] = i;
			ctx->blocks[i]->index = pos;
			++pos;
			length = 0;
			j = i;
			if (ctx->blocks[i]->similar == 0) {
				while (i< blknum && ctx->blocks[i]->similar == 0 &&
				   length < RABIN_MAX_BLOCK_SIZE) {
					length += ctx->blocks[i]->length;
					++i;
					DEBUG_STAT_EN(++merge_count);
				}
				ctx->blocks[j]->length = length;
			} else {
				++i;
			}
		}
		DEBUG_STAT_EN(fprintf(stderr, "Merge count: %u\n", merge_count));

		/*
		 * Final pass update dedupe index and copy data.
		 */
		blknum = pos;
		dedupe_index_sz = (uint64_t)blknum * RABIN_ENTRY_SIZE;
		pos1 = dedupe_index_sz + RABIN_HDR_SIZE;
		matchlen = ctx->real_chunksize - *size;
		for (i=0; i<blknum; i++) {
			be = ctx->blocks[dedupe_index[i]];
			if (be->similar == 0 || be->similar == SIMILAR_REF) {
				/* Just copy. */
				dedupe_index[i] = htonl(be->length);
				memcpy(ctx->cbuf + pos1, buf1 + be->offset, be->length);
				pos1 += be->length;
			} else {
				if (be->similar == SIMILAR_EXACT) {
					dedupe_index[i] = htonl((be->other->index | RABIN_INDEX_FLAG) &
					    CLEAR_SIMILARITY_FLAG);
				} else {
					uchar_t *oldbuf, *newbuf;
					int32_t bsz;
					/*
					 * Perform bsdiff.
					 */
					oldbuf = buf1 + be->other->offset;
					newbuf = buf1 + be->offset;
					DEBUG_STAT_EN(++delta_calls);

					bsz = bsdiff(oldbuf, be->other->length, newbuf, be->length,
					    ctx->cbuf + pos1, buf1 + *size, matchlen);
					if (bsz == 0) {
						DEBUG_STAT_EN(++delta_fails);
						memcpy(ctx->cbuf + pos1, newbuf, be->length);
						dedupe_index[i] = htonl(be->length);
						pos1 += be->length;
					} else {
						dedupe_index[i] = htonl(be->other->index |
						    RABIN_INDEX_FLAG | SET_SIMILARITY_FLAG);
						pos1 += bsz;
					}
				}
			}
		}

		if (valid) {
			uchar_t *cbuf = ctx->cbuf;
			uint64_t *entries;
			DEBUG_STAT_EN(uint64_t sz);

			DEBUG_STAT_EN(sz = *size);
			*((uint32_t *)cbuf) = htonl(blknum);
			cbuf += sizeof (uint32_t);
			entries = (uint64_t *)cbuf;
			entries[0] = htonll(*size);
			entries[1] = 0;
			entries[2] = htonll(pos1 - dedupe_index_sz - RABIN_HDR_SIZE);
			*size = pos1;
			ctx->valid = 1;
			DEBUG_STAT_EN(en = get_wtime_millis());
			DEBUG_STAT_EN(fprintf(stderr, "Deduped size: %" PRId64 ", blknum: %u, delta_calls: %u, delta_fails: %u\n",
					     *size, blknum, delta_calls, delta_fails));
			DEBUG_STAT_EN(fprintf(stderr, "Chunking speed %.3f MB/s, Overall Dedupe speed %.3f MB/s\n",
					      get_mb_s(sz, strt, en_1), get_mb_s(sz, strt, en)));
			/*
			 * Remaining header entries: size of compressed index and size of
			 * compressed data are inserted later via rabin_update_hdr, after actual compression!
			 */
			return (dedupe_index_sz);
		}
	}
	return (0);
}

void
update_dedupe_hdr(uchar_t *buf, uint64_t dedupe_index_sz_cmp, uint64_t dedupe_data_sz_cmp)
{
	uint64_t *entries;

	buf += sizeof (uint32_t);
	entries = (uint64_t *)buf;
	entries[1] = htonll(dedupe_index_sz_cmp);
	entries[3] = htonll(dedupe_data_sz_cmp);
}

void
parse_dedupe_hdr(uchar_t *buf, uint32_t *blknum, uint64_t *dedupe_index_sz,
		uint64_t *dedupe_data_sz, uint64_t *dedupe_index_sz_cmp,
		uint64_t *dedupe_data_sz_cmp, uint64_t *deduped_size)
{
	uint64_t *entries;

	*blknum = ntohl(*((uint32_t *)(buf)));
	buf += sizeof (uint32_t);

	entries = (uint64_t *)buf;
	*dedupe_data_sz = ntohll(entries[0]);
	*dedupe_index_sz = (uint64_t)(*blknum) * RABIN_ENTRY_SIZE;
	*dedupe_index_sz_cmp =  ntohll(entries[1]);
	*deduped_size = ntohll(entries[2]);
	*dedupe_data_sz_cmp = ntohll(entries[3]);
}

void
dedupe_decompress(dedupe_context_t *ctx, uchar_t *buf, uint64_t *size)
{
	uint32_t blknum, blk, oblk, len;
	uint32_t *dedupe_index;
	uint64_t data_sz, sz, indx_cmp, data_sz_cmp, deduped_sz;
	uint64_t dedupe_index_sz, pos1;
	uchar_t *pos2;

	parse_dedupe_hdr(buf, &blknum, &dedupe_index_sz, &data_sz, &indx_cmp, &data_sz_cmp, &deduped_sz);
	dedupe_index = (uint32_t *)(buf + RABIN_HDR_SIZE);
	pos1 = dedupe_index_sz + RABIN_HDR_SIZE;
	pos2 = ctx->cbuf;
	sz = 0;
	ctx->valid = 1;

	slab_cache_add(sizeof (rabin_blockentry_t));
	for (blk = 0; blk < blknum; blk++) {
		if (ctx->blocks[blk] == 0)
			ctx->blocks[blk] = (rabin_blockentry_t *)slab_alloc(NULL, sizeof (rabin_blockentry_t));
		len = ntohl(dedupe_index[blk]);
		ctx->blocks[blk]->hash = 0;
		if (len == 0) {
			ctx->blocks[blk]->hash = 1;

		} else if (!(len & RABIN_INDEX_FLAG)) {
			ctx->blocks[blk]->length = len;
			ctx->blocks[blk]->offset = pos1;
			pos1 += len;
		} else {
			bsize_t blen;

			ctx->blocks[blk]->length = 0;
			if (len & GET_SIMILARITY_FLAG) {
				ctx->blocks[blk]->offset = pos1;
				ctx->blocks[blk]->index = (len & RABIN_INDEX_VALUE) | SET_SIMILARITY_FLAG;
				blen = get_bsdiff_sz(buf + pos1);
				pos1 += blen;
			} else {
				ctx->blocks[blk]->index = len & RABIN_INDEX_VALUE;
			}
		}
	}

	for (blk = 0; blk < blknum; blk++) {
		int rv;
		bsize_t newsz;

		if (ctx->blocks[blk]->hash == 1) continue;
		if (ctx->blocks[blk]->length > 0) {
			len = ctx->blocks[blk]->length;
			pos1 = ctx->blocks[blk]->offset;
		} else {
			oblk = ctx->blocks[blk]->index;

			if (oblk & GET_SIMILARITY_FLAG) {
				oblk = oblk & CLEAR_SIMILARITY_FLAG;
				len = ctx->blocks[oblk]->length;
				pos1 = ctx->blocks[oblk]->offset;
				newsz = data_sz - sz;
				rv = bspatch(buf + ctx->blocks[blk]->offset, buf + pos1, len, pos2, &newsz);
				if (rv == 0) {
					fprintf(stderr, "Failed to bspatch block.\n");
					ctx->valid = 0;
					break;
				}
				pos2 += newsz;
				sz += newsz;
				if (sz > data_sz) {
					fprintf(stderr, "Dedup data overflows chunk.\n");
					ctx->valid = 0;
					break;
				}
				continue;
			} else {
				len = ctx->blocks[oblk]->length;
				pos1 = ctx->blocks[oblk]->offset;
			}
		}
		memcpy(pos2, buf + pos1, len);
		pos2 += len;
		sz += len;
		if (sz > data_sz) {
			fprintf(stderr, "Dedup data overflows chunk.\n");
			ctx->valid = 0;
			break;
		}
	}
	if (ctx->valid && sz < data_sz) {
		fprintf(stderr, "Too little dedup data processed.\n");
		ctx->valid = 0;
	}
	*size = data_sz;
}

/*
 * TODO: Consolidate rabin dedup and compression/decompression in functions here rather than
 * messy code in main program.
int
rabin_compress(dedupe_context_t *ctx, uchar_t *from, uint64_t fromlen, uchar_t *to, uint64_t *tolen,
    int level, char chdr, void *data, compress_func_ptr cmp)
{
}
*/