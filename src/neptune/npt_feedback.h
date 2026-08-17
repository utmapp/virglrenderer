/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-context shared-memory feedback substrate.  One registry, one
 * pending list, one rate-limited poll loop, dispatched by
 * `entry->type` to a per-type poller.  Three sections, both here and
 * in the matching .c:
 *   1. Substrate — registry, pending list, poll loop, generic ops.
 *   2. Query feedback — D3D11 ID3D11Query lifecycle and poller.
 *   3. Fence feedback — ID3D11Fence lifecycle and poller.
 *
 * Per-type modules own the lifecycle hooks (queries: Begin/End in
 * npt_overrides_d3d11_query.c; fences: Signal in
 * npt_overrides_d3d11_fence.c) plus a static poll function dispatched
 * by the substrate.
 */
#ifndef NPT_FEEDBACK_H
#define NPT_FEEDBACK_H

#include <stdbool.h>
#include <stdint.h>

#include "c11/threads.h"
#include "util/list.h"

struct hash_table;
struct npt_context;
struct npt_resource;

/* ================================================================== */
/* Substrate                                                           */
/* ================================================================== */

enum npt_feedback_type {
   NPT_FEEDBACK_TYPE_QUERY   = 1,
   NPT_FEEDBACK_TYPE_FENCE   = 2,  /* ID3D11Fence */
   NPT_FEEDBACK_TYPE_FENCE12 = 3,  /* ID3D12Fence (rewind-capable) */
};

/* Per-object registry entry.  Created on REGISTER_*_FEEDBACK, looked
 * up on per-type lifecycle ops (queries: Begin/End; fences: Signal),
 * freed on UNREGISTER and context destroy.
 *
 * Field semantics:
 *   obj_id      Guest-allocated id; also the object_table key.
 *   type        NPT_FEEDBACK_TYPE_*; selects the per-type poller.
 *   fb_res_id   Shmem resource holding the slot.
 *   fb_offset   Byte offset of the slot inside fb_res_id.
 *   slot_size   Bytes the host writes per successful poll.
 *   host_obj    Cached host-library COM pointer.  Resolved lazily on
 *               first per-type lifecycle op.
 *   host_ctx    Queries: cached ID3D11DeviceContext; fences: unused.
 *   cookie      Type-specific scalar.  Queries: query_data_size bytes.
 *               Fences: unused.
 *   version     Queries only: monotonic per-Begin counter stamped
 *               into the slot so a poll racing Begin sees a mismatch.
 *               Fences: the slot's completed_value plays the version
 *               role itself.
 *   persistent  True for fences: the entry lives in the TABLE for the
 *               object's lifetime (until UNREGISTER).  This says nothing
 *               about the pending list -- see target_value.  False for
 *               queries (one Begin/End cycle = one poll = one removal).
 *   target_value Fences only: highest value passed to Signal.  The
 *               entry leaves the pending list once completed_value
 *               reaches it, so pending_count means "polls still owed"
 *               rather than "a fence exists"; the next Signal re-arms it.
 *   published_high Fences only: highest value ever written to the slot,
 *               used to notice a rewind -- see `rewound`.
 *   rewound     Fences only, sticky: the host observed the fence go
 *               backwards, which D3D12 permits.  Such an entry stops
 *               owing polls, and the guest reads that fence over the
 *               wire instead of from its slot.
 *   pending     True while in the per-state pending list.
 *   pending_head List node into npt_feedback_state::pending.
 */
struct npt_feedback_entry {
   uint64_t obj_id;
   uint8_t  type;
   uint8_t  pad8[3];

   uint32_t fb_res_id;
   uint32_t fb_offset;
   uint32_t slot_size;

   void    *host_obj;
   void    *host_ctx;

   uint32_t cookie;
   uint32_t version;

   uint64_t target_value;
   uint64_t published_high;

   bool persistent;
   bool pending;
   bool rewound;
   struct list_head pending_head;
};

/* Per-context feedback state, embedded in npt_context.  One mutex
 * guards table + pending list together: the dispatch thread is the
 * only slot writer, but COM_RELEASE-driven unregister can fire from
 * either ring or context dispatch, so register / unregister / pending
 * updates serialise here. */
struct npt_feedback_state {
   mtx_t mutex;
   struct hash_table *table;       /* obj_id -> npt_feedback_entry */

   struct list_head pending;
   uint32_t pending_count;

   /* Last poll wall-clock; drives the rate-limit. */
   uint64_t last_poll_ns;
   uint64_t total_poll_ns;
   uint64_t poll_count;
};

void npt_feedback_init(struct npt_context *ctx);
void npt_feedback_fini(struct npt_context *ctx);

/* Insert (or update) an entry for (obj_id, type).  Returns the entry
 * pointer with the state mutex held; caller MUST set type-specific
 * fields (host_obj, host_ctx, cookie, version, persistent) and then
 * call npt_feedback_state_unlock(ctx).  Returns NULL on OOM with the
 * mutex NOT held.
 *
 * Re-register on the same obj_id drops pending state, refreshes slot
 * info, and returns the same entry. */
struct npt_feedback_entry *
npt_feedback_register_locked(struct npt_context *ctx,
                             uint64_t obj_id,
                             uint8_t type,
                             uint32_t fb_res_id,
                             uint32_t fb_offset,
                             uint32_t slot_size);

/* Type-agnostic unregister.  No-op when no entry maps to obj_id, so
 * COM_RELEASE on a non-feedback object is cheap and safe. */
void npt_feedback_unregister(struct npt_context *ctx, uint64_t obj_id);

/* Manual lock/unlock for lifecycle ops that lookup + mutate an entry
 * atomically (queries' mark_end, fences' Signal hook). */
void npt_feedback_state_lock(struct npt_context *ctx);
void npt_feedback_state_unlock(struct npt_context *ctx);

/* Caller holds state lock.  Returns NULL on miss. */
struct npt_feedback_entry *
npt_feedback_lookup_locked(struct npt_context *ctx, uint64_t obj_id);

/* Add the entry to the pending list (no-op if already pending).
 * Caller holds state lock. */
void npt_feedback_set_pending_locked(struct npt_context *ctx,
                                     struct npt_feedback_entry *entry);

/* Remove from pending list (no-op if not pending).  Caller holds
 * state lock. */
void npt_feedback_clear_pending_locked(struct npt_context *ctx,
                                       struct npt_feedback_entry *entry);

/* First entry matching host_obj and type, or NULL.  Linear scan;
 * registered counts are small.  Caller holds state lock. */
struct npt_feedback_entry *
npt_feedback_lookup_by_host_obj_locked(struct npt_context *ctx,
                                       void *host_obj,
                                       uint8_t type);

/* Bounds-checked slot pointer.  Returns NULL when the resource isn't
 * a SHM blob, the offset overflows the resource, or the slot extends
 * past the end. */
void *
npt_feedback_slot_ptr(struct npt_resource *res,
                      uint32_t offset,
                      uint32_t size);

/* The poll interval is the resolution of every value the guest reads
 * out of a feedback slot, so it directly quantizes the app's
 * Signal-to-GetCompletedValue latency.  The same interval is used
 * mid-stream and idle: publishing promptly during decode releases the
 * app's pacing waits earlier, which is worth more than the poll costs
 * the decode thread. */
#define NPT_FEEDBACK_POLL_INTERVAL_NS      (100000ull)
#define NPT_FEEDBACK_POLL_IDLE_INTERVAL_NS (100000ull)

/* How many decoded commands may pass between deadline checks on the
 * mid-stream path.  The poll runs after every command and commands
 * decode in ~1 us, so the clock read alone is a significant share of the
 * decode thread; checking once per 32 holds the effective cadence within
 * a few microseconds of the target at a thirty-second of the clock
 * traffic.  The idle path (npt_feedback_poll_interval) does not skip and
 * stays exact. */
#define NPT_FEEDBACK_POLL_CHECK_EVERY 32u

/* Poll all pending entries (rate-limited internally).  Called from the
 * dispatch loop between commands; no-op on an empty pending list, and
 * only checks the deadline every NPT_FEEDBACK_POLL_CHECK_EVERY calls.
 * `skip` is the CALLER's (per-ring) countdown; a context-wide counter
 * would be a shared cache line written by every ring thread on every
 * command. */
void npt_feedback_poll(struct npt_context *ctx, uint32_t *skip);

/* Same, but with an explicit rate-limit -- the ring thread's idle loop
 * passes NPT_FEEDBACK_POLL_IDLE_INTERVAL_NS. */
void npt_feedback_poll_interval(struct npt_context *ctx,
                                uint64_t min_interval_ns);

/* ================================================================== */
/* Query feedback                                                      */
/* ================================================================== */

void npt_feedback_query_register(struct npt_context *ctx,
                                 uint64_t query_id,
                                 uint32_t fb_res_id,
                                 uint32_t fb_offset,
                                 uint32_t query_data_size);

/* Called from the DC End dispatch override; both args are
 * already-resolved host pointers.  No-op on miss. */
void npt_feedback_query_mark_end(struct npt_context *ctx,
                                 void *host_ctx,
                                 void *host_query);

/* ================================================================== */
/* Fence feedback                                                      */
/* ================================================================== */

/* fence_api: NPT_FENCE_FEEDBACK_API_D3D11 or _D3D12 (selects the
 * GetCompletedValue vtable the poll calls). */
void npt_feedback_fence_register(struct npt_context *ctx,
                                 uint64_t fence_id,
                                 uint32_t fb_res_id,
                                 uint32_t fb_offset,
                                 uint32_t fence_api);

/* Called from the Signal dispatch overrides after the host call
 * succeeds; host_fence is already resolved by the dispatcher.  Matches entries of either fence type.  `value` is the
 * value being signalled: it raises the entry's target_value, and the
 * poller drops the entry from pending once GetCompletedValue reaches
 * it.  Wakes the context's ring threads so an entry armed off the ring
 * thread can never be left unpolled.  No-op on miss, and on a fence the
 * poller has latched as rewound. */
void npt_feedback_fence_mark_signal(struct npt_context *ctx,
                                    void *host_fence,
                                    uint64_t value);

#endif /* NPT_FEEDBACK_H */
