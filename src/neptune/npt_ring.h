/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-context Neptune command ring.
 */

#ifndef NPT_RING_H
#define NPT_RING_H

#include "npt_common.h"
#include "npt_context.h"
#include "npt_cs.h"
#include "npt_profile.h"
#include "npt_transport_defs.h"

/* Bounded by the 32-bit head/tail counter. */
#define NPT_RING_BUFFER_MAX_SIZE (16u * 1024 * 1024)

struct npt_ring_layout {
   const struct npt_resource *resource;

   struct npt_region head;
   struct npt_region tail;
   struct npt_region status;
   struct npt_region buffer;
   struct npt_region extra;
};

static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(atomic_uint) == 4,
              "npt_ring_control requires lock-free 32-bit atomic_uint");

struct npt_ring_control {
   volatile atomic_uint *head;
   const volatile atomic_uint *tail;
   volatile atomic_uint *status;
};

struct npt_ring_buffer {
   uint32_t size;
   uint32_t mask;
   uint32_t cur;
   const uint8_t *data;
};

struct npt_ring_extra {
   size_t offset;
   struct npt_region region;

   size_t cached_offset;
   volatile atomic_uint *cached_data;
};

struct npt_ring {
   uint64_t id;
   struct list_head head;

   const struct npt_resource *resource;
   struct npt_ring_control control;
   struct npt_ring_buffer buffer;
   struct npt_ring_extra extra;

   struct npt_cs_encoder encoder;
   struct npt_cs_decoder decoder;

   /* Copy of the context's dispatch with encoder/decoder rebound to
    * this ring's CS pair.  `data` still points at npt_context. */
   struct npt_dispatch_context dispatch;

   struct npt_context *context;

   uint64_t idle_timeout;
   /* Per-ring countdown for npt_feedback_poll (dispatch-thread private). */
   uint32_t feedback_poll_skip;
   void *cmd;

   mtx_t mutex;
   cnd_t cond;
   thrd_t thread;
   atomic_bool started;

   /* Both guarded by ring->mutex: set there by the notify helpers,
    * cleared and re-tested there by the ring thread's idle wait, which
    * is what makes the wake-up unmissable.  Kept apart because they
    * mean different things to that wait -- pending_notify says the
    * guest submitted work, so the thread is back on the hot path;
    * feedback_notify says a feedback entry needs one poll, which the
    * thread does without leaving the idle regime. */
   atomic_bool pending_notify;
   atomic_bool feedback_notify;

   /* Set when CREATE_RING's monitor_report_period_us is nonzero.
    * The per-context monitor thread OR-sets ALIVE on every flagged ring. */
   atomic_bool monitor;

   /* Best-effort: setpriority() may fail silently without CAP_SYS_NICE. */
   bool prio_valid;
   int  prio;

   struct npt_profile_ring profile;

   /* Guarded by npt_profile.rings_mutex. */
   struct list_head profile_head;

   /* Guarded by ring->mutex/cond.  64-bit to avoid wraparound. */
   uint64_t virtqueue_seqno;
};

/* Caller holds ring->mutex. */
static inline uint64_t
npt_ring_load_virtqueue_seqno(const struct npt_ring *ring)
{
   return ring->virtqueue_seqno;
}

/* Monotonic: never rewinds. */
void
npt_ring_store_virtqueue_seqno(struct npt_ring *ring, uint64_t seqno);

/* False if context fatal'd or ring was stopped during the wait. */
bool
npt_ring_wait_virtqueue_seqno(struct npt_ring *ring, uint64_t seqno);

struct npt_ring *
npt_ring_create(const struct npt_ring_layout *layout,
                struct npt_context *ctx,
                uint64_t idle_timeout);

void
npt_ring_destroy(struct npt_ring *ring);

void
npt_ring_start(struct npt_ring *ring);

bool
npt_ring_stop(struct npt_ring *ring);

/* Guest doorbell: work is in the ring, so the thread leaves the idle
 * regime. */
void
npt_ring_notify(struct npt_ring *ring);

/* A feedback entry owes a poll.  Wakes an idle thread for that one poll
 * without claiming the ring has work, so the idle regime survives. */
void
npt_ring_notify_feedback(struct npt_ring *ring);

/* Wake every waiter on this ring so each re-tests its own predicate.
 * Sets no flag: for state the waiters read directly, such as the
 * context's fatal error. */
void
npt_ring_wake(struct npt_ring *ring);

bool
npt_ring_write_extra(struct npt_ring *ring, size_t offset, uint32_t val);

/* Returns NULL on miss.  The returned pointer is stable for the
 * ring's lifetime; lifetime is bounded by caller serialisation with
 * DESTROY_RING (dispatcher holds ctx->ring_mutex). */
struct npt_ring *
npt_ring_find_by_id(struct npt_context *ctx, uint64_t ring_id);

/* Per-command helpers for multi-step lifecycle ops.  Each takes
 * already-decoded args and returns true on success; false on failure
 * with the reason logged. */

bool
npt_ring_create_from_cmd(struct npt_context *ctx,
                         const struct npt_cmd_create_ring *cmd);

bool
npt_ring_destroy_by_id(struct npt_context *ctx, uint64_t ring_id);

static inline uint32_t
npt_ring_load_head(const struct npt_ring *ring)
{
   return atomic_load_explicit(ring->control.head, memory_order_acquire);
}

static inline void
npt_ring_set_status_bits(struct npt_ring *ring, uint32_t mask)
{
   atomic_fetch_or_explicit(ring->control.status, mask, memory_order_seq_cst);
}

#endif /* NPT_RING_H */
