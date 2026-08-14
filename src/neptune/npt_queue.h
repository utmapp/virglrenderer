/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-ring sync queue.  A worker thread processes a list of
 * (sync_fd, ring_idx, fence_id) tuples and calls ctx->retire_fence
 * on each in order.  We don't own a Vulkan queue: the GPU work is
 * performed by the host D3D library; we wait on the sync_file fd it
 * hands us via the present callback.
 */

#ifndef NPT_QUEUE_H
#define NPT_QUEUE_H

#include "npt_common.h"

struct npt_context;
struct npt_event_proxy;

struct npt_queue_sync {
   /* sync-file fd carrying the GPU-done payload.  -1 means no GPU
    * work to wait on (e.g. CPU-timeline fence) and the worker
    * retires immediately.  Closed by the worker after the wait. */
   int sync_fd;

   uint32_t ring_idx;
   uint64_t fence_id;

   /* Non-NULL: an AUTO_RELEASE arm's transferred proxy reference; the
    * worker releases it after the fence retires, so the signal handle
    * the D3D library stored stays valid until it has been written.
    * Held by pointer: the token may already map to a newer proxy. */
   struct npt_event_proxy *release_proxy;

   /* Value-gated retirement (GATE_WAIT): retire only once
    * GetCompletedValue(check_fence) >= check_value.  check_fence carries
    * an IUnknown reference released at retirement. */
   void *check_fence;
   uint64_t check_value;
   /* Out-of-order wakeup observed: switch to short-period polling so a
    * fire consumed by an earlier gate on the same ring can't stall this
    * one for a full poll period. */
   bool fast_poll;

   /* CLOCK_MONOTONIC instant past which the worker gives up and retires
    * the fence as device-lost rather than trap the guest forever.
    * Stamped at submit, not on reaching the head of the queue, so it
    * measures how long the guest has actually been waiting and a wedged
    * ring unwinds its whole backlog inside one budget. */
   uint64_t deadline_ns;

   struct list_head head;
};

struct npt_queue {
   struct npt_context *context;
   uint32_t ring_idx;

   struct {
      mtx_t mutex;
      cnd_t cond;
      struct list_head syncs;
      thrd_t thread;
      bool join;
   } sync_thread;
};

struct npt_queue *
npt_queue_create(struct npt_context *ctx, uint32_t ring_idx);

void
npt_queue_destroy(struct npt_queue *queue);

struct npt_event_paired;
bool
npt_queue_sync_submit(struct npt_queue *queue,
                      uint32_t ring_idx,
                      uint64_t fence_id,
                      const struct npt_event_paired *paired);

#endif /* NPT_QUEUE_H */
