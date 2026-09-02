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

   /* Ring threads blocked in npt_ring_wait_peer_seqno on THIS ring's
    * decode position, and the lowest position any of them waits for.
    * The decode loop reads both (relaxed, ring-private line) after every
    * head publish and only takes ring->mutex to broadcast once the head
    * reaches that position, so an unwatched ring pays one load per
    * command and a watched one one broadcast per satisfied wait.  The
    * list is guarded by ring->mutex. */
   atomic_uint peer_waiters;
   atomic_uint peer_wake_at;
   struct list_head peer_waits;
   /* Threads that resolved this ring by id and may still touch it;
    * taken under ctx->ring_mutex, which destroy_by_id waits out. */
   atomic_uint peer_pins;

   /* Deferred COM_RELEASEs waiting for THIS ring's head to pass a
    * position, sorted by that position (snapshots of the tail are taken
    * in decode order).  The decode loop checks the first entry after
    * every head publish; guarded by watch_mutex.  A destroyed ring
    * settles every entry (its bytes were drained by DESTROY_RING). */
   mtx_t watch_mutex;
   struct list_head watch;
   atomic_bool watch_pending;
};

/* One ring's share of a deferred release: settled once the ring's head
 * passes seqno (npt_context_deferred_release_settle). */
struct npt_ring_watch {
   struct list_head head;
   struct npt_deferred_release *release;
   uint32_t seqno;
};

struct npt_ring_peer_wait {
   struct list_head head;
   uint32_t seqno;
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

struct npt_ring *
npt_ring_lookup_wait_locked(struct npt_context *ctx, uint64_t ring_id);

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

static inline uint32_t
npt_ring_load_tail(const struct npt_ring *ring)
{
   return atomic_load_explicit(ring->control.tail, memory_order_acquire);
}

/* The ring whose thread is decoding `dispatch` (a copy of the context
 * dispatch rebound to that ring's CS pair). */
static inline struct npt_ring *
npt_ring_from_dispatch(struct npt_dispatch_context *dispatch)
{
   return (struct npt_ring *)((uint8_t *)dispatch -
                              offsetof(struct npt_ring, dispatch));
}

/* The ring whose decode thread this is, or NULL on the context
 * (virtqueue) thread and everything else.  Waits that may take
 * arbitrarily long -- a peer ring's decode position, an object another
 * ring has yet to register -- are allowed only on a ring thread: the
 * context thread runs inside QEMU's synchronous control-queue handling,
 * and blocking it stalls every vCPU of the guest.  Such waits also end
 * when the ring is stopped, so a DESTROY_RING can join it. */
struct npt_ring *
npt_ring_current(void);

/* Block the calling ring thread until ring target_id's decode position
 * reaches seqno.  Returns once satisfied, once the target stops or is
 * gone (a destroyed ring was drained by its DESTROY_RING, so the edge
 * is met), or once the context goes fatal.  The target is resolved and
 * pinned against destruction under ctx->ring_mutex. */
void
npt_ring_wait_peer_seqno(struct npt_context *ctx, struct npt_ring *self,
                         uint64_t target_id, uint32_t seqno);

/* Register a deferred-release watch on `ring` (caller holds
 * ctx->ring_mutex, which keeps the ring alive). */
void
npt_ring_watch_add(struct npt_ring *ring, struct npt_ring_watch *watch);

/* Settle the watches `head` has reached; called by the decode loop. */
void
npt_ring_settle_watches(struct npt_ring *ring, uint32_t head);

static inline void
npt_ring_set_status_bits(struct npt_ring *ring, uint32_t mask)
{
   atomic_fetch_or_explicit(ring->control.status, mask, memory_order_seq_cst);
}

#endif /* NPT_RING_H */
