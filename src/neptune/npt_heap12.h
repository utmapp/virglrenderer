/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 persistently-mapped heap support.
 */

#ifndef NPT_HEAP12_H
#define NPT_HEAP12_H

#include "npt_common.h"

struct npt_context;
struct npt_cs_decoder;
struct npt_cs_encoder;
struct npt_command_header;

/* Handler for NPT_TRANSPORT_RESOURCE_CREATE_HEAP_FROM_SHMEM. */
void
npt_dispatch_create_heap_from_shmem(struct npt_context *ctx,
                                    struct npt_cs_decoder *dec,
                                    struct npt_cs_encoder *enc,
                                    const struct npt_command_header *header);

/* COM_RELEASE hook: if guest_id is a shmem-imported heap, drop the
 * backing resource's pin and complete a deferred zombie
 * munmap.  Must run AFTER the host library released the heap object.
 * Silent no-op for non-heap ids. */
void
npt_heap12_on_release_object(struct npt_context *ctx, uint64_t guest_id);

/* Context teardown: free zombie resources kept alive only by the
 * import table.  Call BEFORE resource_table destruction. */
void
npt_heap12_context_fini(struct npt_context *ctx);

#endif /* NPT_HEAP12_H */
