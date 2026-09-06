/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-command helpers for RESOURCE_{UPDATE, MAP, UNMAP}.  D3D11
 * resource manipulation lives here so npt_dispatch.c can stay a
 * decode/encode shell.
 */

#ifndef NPT_RESOURCE_H
#define NPT_RESOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "npt_com.h"

struct npt_context;

/* Hard cap on per-call payload byte_size: 2 GB, the guest client's map
 * slot cap, and bounded so the byte_size+7 alignment math can't wrap to 0
 * on a hostile value and read stale bytes. */
#define NPT_MAX_RESOURCE_UPDATE_BYTES (2048u << 20)

/* RESOURCE_UPDATE.  `payload` points at the contiguous source bytes in
 * the decoder's stream; lifetime is the caller's responsibility. */
void
npt_resource_update(struct npt_context *ctx,
                    uint64_t resource_id, uint32_t subresource,
                    uint32_t row_pitch, uint32_t depth_pitch,
                    uint32_t byte_size,
                    bool has_box,
                    uint32_t box_left, uint32_t box_top, uint32_t box_front,
                    uint32_t box_right, uint32_t box_bottom, uint32_t box_back,
                    const void *payload);

/* RESOURCE_MAP.  read_range_{begin,end} are accepted for the D3D12
 * path (pReadRange) and ignored on D3D11. */
HRESULT
npt_resource_map(struct npt_context *ctx,
                 uint64_t context_id, uint64_t resource_id,
                 uint32_t subresource, uint32_t access_flags,
                 uint32_t api_map_flags, uint32_t shmem_res_id,
                 uint64_t read_range_begin, uint64_t read_range_end,
                 uint64_t byte_size,
                 uint32_t mip_rows, uint32_t mip_depth,
                 uint32_t shmem_offset,
                 uint32_t *out_row_pitch, uint32_t *out_depth_pitch,
                 uint32_t *out_mapped_size);

/* RESOURCE_UNMAP.  written_range_{begin,end} are accepted for the
 * D3D12 path (pWrittenRange) and ignored on D3D11. */
HRESULT
npt_resource_unmap(struct npt_context *ctx,
                   uint64_t context_id, uint64_t resource_id,
                   uint32_t subresource, uint32_t shmem_res_id,
                   uint32_t shmem_offset, uint64_t byte_size,
                   uint32_t access_flags,
                   uint64_t written_range_begin,
                   uint64_t written_range_end);

#endif /* NPT_RESOURCE_H */
