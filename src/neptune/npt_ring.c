/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_ring.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/resource.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif
#endif

#include "npt_context.h"
#include "npt_cs.h"
#include "npt_feedback.h"  /* NPT_FEEDBACK_POLL_INTERVAL_NS */
#include "npt_library.h"   /* struct npt_d3d_library (workaround_flags) */
#include "npt_profile.h"
#include "npt_dispatch.h"
#include "neptune-protocol/npt_protocol_host_dispatch.h"

static _Thread_local struct npt_ring *npt_ring_thread_current;

struct npt_ring *
npt_ring_current(void)
{
   return npt_ring_thread_current;
}

static inline void *
get_resource_pointer(const struct npt_resource *res, size_t offset)
{
   assert(offset < res->size);
   return res->u.data + offset;
}

static void
npt_ring_init_extra(struct npt_ring *ring, const struct npt_ring_layout *layout)
{
   struct npt_ring_extra *extra = &ring->extra;

   extra->offset = layout->extra.begin;
   extra->region = npt_region_make_relative(&layout->extra);
}

static void
npt_ring_init_buffer(struct npt_ring *ring, const struct npt_ring_layout *layout)
{
   struct npt_ring_buffer *buf = &ring->buffer;

   buf->size = npt_region_size(&layout->buffer);
   assert(util_is_power_of_two_nonzero(buf->size));
   buf->mask = buf->size - 1;
   buf->cur = 0;
   buf->data = get_resource_pointer(layout->resource, layout->buffer.begin);
}

static bool
npt_ring_init_control(struct npt_ring *ring, const struct npt_ring_layout *layout)
{
   struct npt_ring_control *ctrl = &ring->control;

   ctrl->head = get_resource_pointer(layout->resource, layout->head.begin);
   ctrl->tail = get_resource_pointer(layout->resource, layout->tail.begin);
   ctrl->status = get_resource_pointer(layout->resource, layout->status.begin);

   /* head and status are host-owned: reject pre-populated values. */
   if (*ctrl->head || *ctrl->status) {
      npt_log("ring: res %u head@%u=0x%08x status@%u=0x%08x tail@%u=0x%08x "
              "not zero at create (stale ring memory or aliased blob)",
              layout->resource->res_id, (unsigned)layout->head.begin,
              *ctrl->head, (unsigned)layout->status.begin, *ctrl->status,
              (unsigned)layout->tail.begin, *ctrl->tail);
      return false;
   }

   return true;
}

static void
npt_ring_store_head(struct npt_ring *ring, uint32_t ring_head)
{
   atomic_store_explicit(ring->control.head, ring_head, memory_order_release);
}

static void
npt_ring_unset_status_bits(struct npt_ring *ring, uint32_t mask)
{
   atomic_fetch_and_explicit(ring->control.status, ~mask, memory_order_seq_cst);
}

static void
npt_ring_read_buffer(struct npt_ring *ring, void *data, uint32_t size)
{
   struct npt_ring_buffer *buf = &ring->buffer;

   const size_t offset = buf->cur & buf->mask;
   assert(size <= buf->size);
   if (offset + size <= buf->size) {
      memcpy(data, buf->data + offset, size);
   } else {
      const size_t s = buf->size - offset;
      memcpy(data, buf->data + offset, s);
      memcpy((uint8_t *)data + s, buf->data, size - s);
   }

   buf->cur += size;
}

/* dispatch.data stays pointing at the context so override functions
 * recover it the same way from either dispatch path. */
static void
npt_ring_init_dispatch(struct npt_ring *ring, struct npt_context *ctx)
{
   ring->context = ctx;
   ring->dispatch = ctx->dispatch;
   ring->dispatch.encoder = &ring->encoder;
   ring->dispatch.decoder = &ring->decoder;
}

struct npt_ring *
npt_ring_create(const struct npt_ring_layout *layout,
                struct npt_context *ctx,
                uint64_t idle_timeout)
{
   struct npt_ring *ring = calloc(1, sizeof(*ring));
   if (!ring)
      return NULL;

   ring->resource = layout->resource;

   if (!npt_ring_init_control(ring, layout))
      goto err_init_control;

   npt_ring_init_buffer(ring, layout);
   npt_ring_init_extra(ring, layout);

   ring->cmd = malloc(ring->buffer.size);
   if (!ring->cmd)
      goto err_cmd_malloc;

   if (npt_cs_decoder_init(&ring->decoder, &ctx->cs_fatal_error))
      goto err_cs_decoder_init;

   if (npt_cs_encoder_init(&ring->encoder, &ctx->cs_fatal_error))
      goto err_cs_encoder_init;

   npt_ring_init_dispatch(ring, ctx);

   ring->idle_timeout = idle_timeout;

   if (mtx_init(&ring->mutex, mtx_plain) != thrd_success)
      goto err_mtx_init;

   if (cnd_init(&ring->cond) != thrd_success)
      goto err_cond_init;

   if (mtx_init(&ring->watch_mutex, mtx_plain) != thrd_success)
      goto err_watch_mtx_init;

   ring->virtqueue_seqno = 0;
   atomic_store_explicit(&ring->peer_waiters, 0, memory_order_relaxed);
   atomic_store_explicit(&ring->peer_wake_at, 0, memory_order_relaxed);
   atomic_store_explicit(&ring->peer_pins, 0, memory_order_relaxed);
   list_inithead(&ring->peer_waits);
   list_inithead(&ring->watch);
   atomic_store_explicit(&ring->watch_pending, false, memory_order_relaxed);

   /* Safe to call unconditionally; no-op when profiling is disabled. */
   npt_profile_register_ring(ring);

   return ring;

err_watch_mtx_init:
   cnd_destroy(&ring->cond);
err_cond_init:
   mtx_destroy(&ring->mutex);
err_mtx_init:
   npt_cs_encoder_fini(&ring->encoder);
err_cs_encoder_init:
   npt_cs_decoder_fini(&ring->decoder);
err_cs_decoder_init:
   free(ring->cmd);
err_cmd_malloc:
err_init_control:
   free(ring);
   return NULL;
}

void
npt_ring_destroy(struct npt_ring *ring)
{
   list_del(&ring->head);

   assert(!ring->started);
   npt_profile_unregister_ring(ring);
   npt_cs_decoder_fini(&ring->decoder);
   npt_cs_encoder_fini(&ring->encoder);
   mtx_destroy(&ring->watch_mutex);
   mtx_destroy(&ring->mutex);
   cnd_destroy(&ring->cond);
   free(ring->cmd);
   free(ring);
}

void
npt_ring_store_virtqueue_seqno(struct npt_ring *ring, uint64_t seqno)
{
   /* Monotonic.  Broadcast, not signal: the idle wait and every
    * wait_virtqueue_seqno waiter share this mutex/cond and test
    * different predicates, so a single wake could land on a waiter that
    * just sleeps again and swallow it. */
   mtx_lock(&ring->mutex);
   if (seqno > ring->virtqueue_seqno)
      ring->virtqueue_seqno = seqno;
   cnd_broadcast(&ring->cond);
   mtx_unlock(&ring->mutex);
}

bool
npt_ring_wait_virtqueue_seqno(struct npt_ring *ring, uint64_t seqno)
{
   bool ok = true;
   mtx_lock(&ring->mutex);
   while (ok && ring->started && !ring->context->cs_fatal_error &&
          ring->virtqueue_seqno < seqno)
      ok = cnd_wait(&ring->cond, &ring->mutex) == thrd_success;
   const bool stopped = !ring->started || ring->context->cs_fatal_error;
   mtx_unlock(&ring->mutex);

   return ok && !stopped;
}

static uint64_t
npt_ring_now(void)
{
   const uint64_t ns_per_sec = 1000000000llu;
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      return 0;
   return ns_per_sec * now.tv_sec + now.tv_nsec;
}

/* Feedback entries owe the idle-gap poll an advance.  A fence entry
 * clears itself once GetCompletedValue catches up to the signalled
 * target, so a nonzero count means a poll really is outstanding rather
 * than merely "a fence is registered". */
static bool
npt_ring_feedback_pending(const struct npt_context *ctx)
{
   return atomic_load_explicit((_Atomic uint32_t *)&ctx->feedback.pending_count,
                               memory_order_relaxed) != 0;
}

/* Bounded wait on ring->cond with a relative timeout.  Caller holds
 * ring->mutex.
 *
 * c11 cnd_timedwait takes an absolute CLOCK_REALTIME deadline, so a
 * backward wall-clock step lands the deadline that far in the future --
 * and while a feedback poll is owed, that timeout is the only thing that
 * will publish the value a guest thread is blocked on.  Both platform
 * primitives below take a relative timeout off a clock that cannot step.
 * The portable fallback is the c11 one and keeps that exposure. */
static void
npt_ring_cond_wait_ns(struct npt_ring *ring, uint64_t ns)
{
#if defined(_WIN32)
   SleepConditionVariableCS(&ring->cond, &ring->mutex,
                            (DWORD)(ns / 1000000ull));
#else
   const uint64_t ns_per_sec = 1000000000ull;
#if defined(__APPLE__)
   const struct timespec rel = {
      .tv_sec = (time_t)(ns / ns_per_sec),
      .tv_nsec = (long)(ns % ns_per_sec),
   };
   pthread_cond_timedwait_relative_np(&ring->cond, &ring->mutex, &rel);
#else
   struct timespec ts;
   timespec_get(&ts, TIME_UTC);
   ts.tv_sec += (time_t)(ns / ns_per_sec);
   ts.tv_nsec += (long)(ns % ns_per_sec);
   if (ts.tv_nsec >= (long)ns_per_sec) {
      ts.tv_sec += 1;
      ts.tv_nsec -= (long)ns_per_sec;
   }
   cnd_timedwait(&ring->cond, &ring->mutex, &ts);
#endif
#endif
}

static void
npt_ring_relax(uint32_t *iter)
{
   const uint32_t busy_wait_order = 4;
   const uint32_t base_sleep_us = 10;

   (*iter)++;
   if (*iter < (1u << busy_wait_order)) {
      thrd_yield();
      return;
   }

   const uint32_t shift = util_last_bit(*iter) - busy_wait_order - 1;
   const uint32_t us = base_sleep_us << shift;
   const struct timespec ts = {
      .tv_sec = us / 1000000,
      .tv_nsec = (us % 1000000) * 1000,
   };
#ifdef __APPLE__
   nanosleep(&ts, NULL);
#else
   clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
#endif
}

static bool
npt_ring_submit_cmd(struct npt_ring *ring,
                    const uint8_t *buffer,
                    size_t size,
                    uint32_t ring_head)
{
   struct npt_cs_decoder *dec = &ring->decoder;
   if (npt_cs_decoder_get_fatal(dec)) {
      npt_log("ring_submit_cmd: early bail due to fatal decoder state");
      return false;
   }

   npt_cs_decoder_set_buffer_stream(dec, buffer, size);
   const bool prof_enabled = npt_profile_enabled();
   npt_profile_record_read(&ring->profile, (uint32_t)size);

   while (npt_cs_decoder_has_command(dec)) {
      /* Peek for per-method attribution; the dispatcher peeks again. */
      uint32_t cmd_type = 0;
      uint64_t t0 = 0;
      if (prof_enabled) {
         struct npt_command_header peek;
         npt_cs_decoder_peek(dec, sizeof(peek), &peek, sizeof(peek));
         cmd_type = peek.cmd_type;
         t0 = npt_profile_now_ns();
      }

      if (!npt_context_dispatch_one_command(ring->context, &ring->dispatch,
                                            dec, &ring->encoder)) {
         npt_log("ring_submit_cmd: dispatch failed");
         npt_cs_decoder_reset(dec);
         return false;
      }

      if (prof_enabled) {
         const uint64_t dispatch_ns = npt_profile_now_ns() - t0;
         npt_profile_record_dispatch(&ring->profile, cmd_type, dispatch_ns);
         npt_profile_maybe_dump(&ring->profile);
      }

      /* Publish head mid-stream so the producer can refill space. */
      const uint32_t cur_ring_head = ring_head + (dec->cur - buffer);
      npt_ring_store_head(ring, cur_ring_head);

      /* Dekker pairing with npt_ring_wait_peer_seqno: it registers as
       * a waiter, fences, then reads the head; this side publishes the
       * head, fences, then reads the waiter count.  One of the two
       * always sees the other, so a waiter never sleeps on a head that
       * was published without a broadcast. */
      atomic_thread_fence(memory_order_seq_cst);
      if (unlikely(atomic_load_explicit(&ring->peer_waiters,
                                        memory_order_relaxed)) &&
          npt_seqno_ge(cur_ring_head,
                       atomic_load_explicit(&ring->peer_wake_at,
                                            memory_order_relaxed)))
         npt_ring_wake(ring);

      if (unlikely(atomic_load_explicit(&ring->watch_pending,
                                        memory_order_relaxed)))
         npt_ring_settle_watches(ring, cur_ring_head);

      npt_context_on_ring_seqno_update(ring->context, ring->id, cur_ring_head);

      /* A sustained batch keeps the ring busy for seconds at a time, so
       * publishing feedback only once the ring drains would freeze the
       * guest-visible fence values and stall every guest wait keyed on them.
       * This runs on the dispatch thread, keeping the host D3D11 context
       * single-threaded; the poll rate-limits itself and early-outs when
       * nothing is pending. */
      npt_feedback_poll(ring->context, &ring->feedback_poll_skip);
   }

   npt_cs_decoder_reset(dec);
   return true;
}

static int
npt_ring_thread(void *arg)
{
   struct npt_ring *ring = arg;
   struct npt_context *ctx = ring->context;
   char thread_name[16];

   snprintf(thread_name, ARRAY_SIZE(thread_name), "npt-ring-%d", ctx->ctx_id);
   u_thread_setname(thread_name);
   npt_ring_thread_current = ring;

#if defined(__APPLE__)
   /* Darwin has no per-thread nice: setpriority(PRIO_PROCESS, 0, ...)
    * is process-wide here (the "0 means calling thread" behaviour the
    * guest value is meant for is a Linux extension), so honouring the
    * guest's nice would renice the WHOLE render server -- every ring of
    * every context in it -- on behalf of one guest thread.  Set the QoS
    * class instead: it is per-thread and it, not nice, is what decides
    * scheduling band and core placement on macOS.
    *
    * Every ring of a multi-ring guest runs UTILITY, which on Apple silicon
    * confines the thread to the efficiency cores: those threads mostly
    * poll (npt_ring_relax between bursts, the bounded park when idle),
    * there is one per recording thread, and on the performance cores they
    * take time from the vCPU threads that is worth more than their decode
    * speed.  A single-ring guest keeps its one ring USER_INTERACTIVE: that
    * decoder is on the frame's critical path and loses frame time on an
    * efficiency core.  The first ring therefore starts USER_INTERACTIVE
    * and moves itself once the context turns multi-ring (see the loop). */
   ring->qos_utility =
      atomic_load_explicit(&ctx->multi_ring, memory_order_acquire);
   {
      const int qos_rc = pthread_set_qos_class_self_np(
         ring->qos_utility ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INTERACTIVE, 0);
      static _Atomic bool qos_logged;
      if (!atomic_exchange(&qos_logged, true)) {
         qos_class_t got = QOS_CLASS_UNSPECIFIED;
         pthread_get_qos_class_np(pthread_self(), &got, NULL);
         npt_log("ring thread QoS: set rc=%d, effective class=0x%x "
                 "(USER_INTERACTIVE=0x%x UTILITY=0x%x)", qos_rc, (unsigned)got,
                 (unsigned)QOS_CLASS_USER_INTERACTIVE, (unsigned)QOS_CLASS_UTILITY);
      }
   }
#elif !defined(_WIN32)
   if (ring->prio_valid) {
      errno = 0;
      if (setpriority(PRIO_PROCESS, 0, ring->prio) == -1 && errno) {
#ifdef DEBUG
         /* Non-fatal; needs CAP_SYS_NICE for a negative nice value. */
         npt_log("ring thread setpriority(%d) failed: %d",
                 ring->prio, errno);
#endif
      }
   }
#endif

   uint64_t last_submit = npt_ring_now();
   uint32_t relax_iter = 0;
   int ret = 0;
   while (ring->started) {
#if defined(__APPLE__)
      if (unlikely(!ring->qos_utility) &&
          atomic_load_explicit(&ctx->multi_ring, memory_order_acquire)) {
         pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
         ring->qos_utility = true;
      }
#endif
      /* An empty ring past the idle timeout waits; feedback decides only
       * whether the wait is bounded by the poll cadence. */
      bool notified = false;
      if (npt_ring_now() >= last_submit + ring->idle_timeout) {
         const uint64_t idle_t0 =
            npt_profile_enabled() ? npt_profile_now_ns() : 0;

         /* Clearing the flags, reading the tail and entering the wait
          * all happen under ring->mutex -- the lock the notify helpers
          * set those flags under.  That is what makes a notify arriving
          * mid-decision unmissable: it either lands before the clear and
          * is seen here, or it blocks on the mutex until this thread is
          * inside the wait, where the broadcast reaches it. */
         mtx_lock(&ring->mutex);
         ring->pending_notify = false;
         /* A poll is owed if an entry is on the pending list or one was
          * armed off this thread since the last look. */
         const bool poll_owed =
            ring->feedback_notify || npt_ring_feedback_pending(ctx);
         ring->feedback_notify = false;

         /* Dekker pairing with the guest submit path (store tail, then
          * load status): set IDLE with a seq_cst RMW BEFORE deciding
          * from the tail whether to sleep.  The guest orders its tail
          * store before its status load with a full fence, so at least
          * one side always sees the other: either the tail read below
          * sees the new tail and the wait is skipped, or the guest sees
          * IDLE and rings the doorbell, which takes ring->mutex and so
          * cannot land between this decision and the wait.  Deciding
          * from a tail read taken before the IDLE transition lets a
          * submit slip through the gap -- guest misses IDLE (no
          * doorbell), host misses the tail -> the ring parks with work
          * already published. */
         npt_ring_set_status_bits(ring, NPT_RING_STATUS_IDLE_BIT);
         const bool wait = ring->buffer.cur == npt_ring_load_tail(ring);
         if (wait && ring->started) {
            if (poll_owed) {
               /* Sleep straight to the next poll deadline instead of
                * spending the yield/10us relax ramp to get there. */
               npt_ring_cond_wait_ns(ring, NPT_FEEDBACK_POLL_INTERVAL_NS);
            } else {
               /* Bounded park rather than an unbounded one: the ring
                * blob is cached here and uncached in the guest, so the
                * IDLE bit set above reaches the guest only when this
                * side's cache line is written back, and until then a
                * guest submit sees no IDLE and rings no doorbell; the
                * tail can lag the same way in the other direction.  The
                * timeout bounds how long published work can sit behind
                * that, and it is what the guest's ring-space and reply
                * waits see as latency, so it is short. */
               npt_ring_cond_wait_ns(ring, 1000ull * 1000);
            }
         }
         npt_ring_unset_status_bits(ring, NPT_RING_STATUS_IDLE_BIT);
         /* Only a guest doorbell puts the thread back on the hot path.
          * A poll-cadence timeout or a feedback wake must not, or the
          * thread drops out of the idle regime into the relax spin the
          * wait exists to avoid -- so read the doorbell flag rather than
          * the wait's return code, which cannot tell them apart. */
         notified = ring->pending_notify;
         mtx_unlock(&ring->mutex);

         if (wait && npt_profile_enabled())
            npt_profile_record_idle_wait(&ring->profile,
                                         npt_profile_now_ns() - idle_t0);

         if (!ring->started)
            break;
      }

      if (notified) {
         last_submit = npt_ring_now();
         relax_iter = 0;
      }

      const uint32_t cmd_size = npt_ring_load_tail(ring) - ring->buffer.cur;
      if (cmd_size) {
         if (cmd_size > ring->buffer.size) {
            npt_log("%s: cmd_size(%u) > ring->buffer.size(%u)", __func__, cmd_size,
                    ring->buffer.size);
            ret = -EINVAL;
            break;
         }

         const uint32_t ring_head = ring->buffer.cur;
         npt_ring_read_buffer(ring, ring->cmd, cmd_size);

         if (!npt_ring_submit_cmd(ring, ring->cmd, cmd_size, ring_head)) {
            ret = -EINVAL;
            break;
         }

         last_submit = npt_ring_now();
         relax_iter = 0;
      } else {
         /* Buffer empty.  Catch a driver bug where the guest waits
          * on a seqno this ring can't reach. */
         uint32_t wait_ring_seqno = 0;
         if (npt_context_get_wait_ring_seqno(ctx, ring->id,
                                             &wait_ring_seqno)) {
            const uint32_t ring_tail = npt_ring_load_tail(ring);
            if (unlikely(!npt_seqno_ge(ring_tail, wait_ring_seqno))) {
               npt_log("%s: ring seqno(%u) unable to reach wait seqno(%u)",
                       __func__, ring_tail, wait_ring_seqno);
               ret = -EINVAL;
               break;
            }
         }

         /* Drain pending feedback while idle.  The host D3D11
          * context is single-threaded — interleaving feedback polls
          * with batched draws would break the host driver, so the
          * poll has to land in the idle gap.  Idle polls use the
          * short interval: nothing is being decoded, so the only
          * cost is the poll itself, and this is what bounds guest
          * GetCompletedValue latency on a quiet ring. */
         npt_feedback_poll_interval(ctx, NPT_FEEDBACK_POLL_IDLE_INTERVAL_NS);

         /* Cap relax_iter while feedback is pending: GPU execution
          * of a queued Signal lags by ms, so longer sleeps would
          * miss the value advancing.  16 keeps the next sleep ~10 us. */
         if (npt_ring_feedback_pending(ctx) && relax_iter > 16)
            relax_iter = 16;

         const uint64_t relax_t0 =
            npt_profile_enabled() ? npt_profile_now_ns() : 0;
         npt_ring_relax(&relax_iter);
         if (npt_profile_enabled())
            npt_profile_record_relax(&ring->profile,
                                     npt_profile_now_ns() - relax_t0);
      }
   }

   if (ret < 0) {
      npt_ring_set_status_bits(ring, NPT_RING_STATUS_FATAL_BIT);
      npt_context_on_ring_fatal(ring->context);
   }

   return ret;
}

void
npt_ring_start(struct npt_ring *ring)
{
   int ret;

   assert(!ring->started);
   ring->started = true;
   ret = thrd_create(&ring->thread, npt_ring_thread, ring);
   if (ret != thrd_success)
      ring->started = false;
}

bool
npt_ring_stop(struct npt_ring *ring)
{
   mtx_lock(&ring->mutex);
   /* Nothing to join: ring->thread is only valid once npt_ring_start
    * has run, and joining the zero value would fault. */
   if (!ring->started) {
      mtx_unlock(&ring->mutex);
      return true;
   }
   if (thrd_equal(ring->thread, thrd_current())) {
      mtx_unlock(&ring->mutex);
      return false;
   }
   ring->started = false;
   /* Broadcast: the ring thread and any wait_virtqueue_seqno waiter
    * both have to observe the cleared started flag. */
   cnd_broadcast(&ring->cond);
   mtx_unlock(&ring->mutex);

   thrd_join(ring->thread, NULL);

   return true;
}

void
npt_ring_notify(struct npt_ring *ring)
{
   mtx_lock(&ring->mutex);
   ring->pending_notify = true;
   cnd_broadcast(&ring->cond);
   mtx_unlock(&ring->mutex);
}

void
npt_ring_notify_feedback(struct npt_ring *ring)
{
   mtx_lock(&ring->mutex);
   ring->feedback_notify = true;
   cnd_broadcast(&ring->cond);
   mtx_unlock(&ring->mutex);
}

void
npt_ring_wake(struct npt_ring *ring)
{
   mtx_lock(&ring->mutex);
   cnd_broadcast(&ring->cond);
   mtx_unlock(&ring->mutex);
}

static void
npt_ring_update_peer_wake_at(struct npt_ring *ring);


void
npt_ring_wait_peer_seqno(struct npt_context *ctx, struct npt_ring *self,
                         uint64_t target_id, uint32_t seqno)
{
   const uint64_t report_ns = 5000000000ull;
   const uint64_t t0 = npt_ring_now();
   uint64_t next_report = t0 + report_ns;

   /* Register as a waiter under the list lock: destroy_by_id drops a
    * ring only once it sees no waiters under that same lock, so from
    * here until the decrement the target cannot go away. */
   mtx_lock(&ctx->ring_mutex);
   struct npt_ring *target = npt_ring_lookup_wait_locked(ctx, target_id);
   if (target)
      atomic_fetch_add_explicit(&target->peer_pins, 1, memory_order_relaxed);
   mtx_unlock(&ctx->ring_mutex);
   if (!target)
      return;

   mtx_lock(&target->mutex);
   struct npt_ring_peer_wait wait = { .seqno = seqno };
   list_addtail(&wait.head, &target->peer_waits);
   npt_ring_update_peer_wake_at(target);
   /* Only now, with the wake position published, announce the waiter:
    * the decode loop reads the count first and the position second, so
    * a count it sees always comes with a position that covers this
    * wait.  The count must then be visible to the loop before this
    * thread reads the head it might already have published; the loop
    * pairs the same way (publish head, then read the count). */
   atomic_fetch_add_explicit(&target->peer_waiters, 1, memory_order_relaxed);
   atomic_thread_fence(memory_order_seq_cst);
   while (!npt_seqno_ge(npt_ring_load_head(target), seqno) &&
          target->started && self->started && !ctx->cs_fatal_error) {
      npt_ring_cond_wait_ns(target, 10ull * 1000 * 1000);
      const uint64_t now = npt_ring_now();
      if (now >= next_report) {
         npt_log("wait_peer: ring %" PRIu64 " has waited %" PRIu64
                 " ms for ring %" PRIu64 " to reach %u (head=%u tail=%u)",
                 self->id, (now - t0) / 1000000, target->id, seqno,
                 npt_ring_load_head(target), npt_ring_load_tail(target));
         next_report = now + report_ns;
      }
   }
   list_del(&wait.head);
   npt_ring_update_peer_wake_at(target);
   atomic_fetch_sub_explicit(&target->peer_waiters, 1, memory_order_relaxed);
   mtx_unlock(&target->mutex);
   atomic_fetch_sub_explicit(&target->peer_pins, 1, memory_order_release);
}

/* Caller holds ring->mutex. */
static void
npt_ring_update_peer_wake_at(struct npt_ring *ring)
{
   bool any = false;
   uint32_t min = 0;
   list_for_each_entry(struct npt_ring_peer_wait, w, &ring->peer_waits, head) {
      if (!any || !npt_seqno_ge(w->seqno, min))
         min = w->seqno;
      any = true;
   }
   atomic_store_explicit(&ring->peer_wake_at, min, memory_order_relaxed);
}

void
npt_ring_watch_add(struct npt_ring *ring, struct npt_ring_watch *watch)
{
   mtx_lock(&ring->watch_mutex);
   list_addtail(&watch->head, &ring->watch);
   atomic_store_explicit(&ring->watch_pending, true, memory_order_release);
   mtx_unlock(&ring->watch_mutex);
}

/* Move every watch whose position `head` has reached (all of them when
 * `all`) onto `settled`. */
static void
npt_ring_detach_watches(struct npt_ring *ring, uint32_t head, bool all,
                        struct list_head *settled)
{
   mtx_lock(&ring->watch_mutex);
   list_for_each_entry_safe(struct npt_ring_watch, w, &ring->watch, head) {
      if (!all && !npt_seqno_ge(head, w->seqno))
         break;
      list_del(&w->head);
      list_addtail(&w->head, settled);
   }
   if (list_is_empty(&ring->watch))
      atomic_store_explicit(&ring->watch_pending, false,
                            memory_order_relaxed);
   mtx_unlock(&ring->watch_mutex);
}

static void
npt_ring_settle_list(struct npt_context *ctx, struct list_head *settled)
{
   list_for_each_entry_safe(struct npt_ring_watch, w, settled, head) {
      list_del(&w->head);
      npt_context_deferred_release_settle(ctx, w->release);
   }
}

void
npt_ring_settle_watches(struct npt_ring *ring, uint32_t head)
{
   struct list_head settled;
   list_inithead(&settled);
   npt_ring_detach_watches(ring, head, false, &settled);
   npt_ring_settle_list(ring->context, &settled);
}

bool
npt_ring_write_extra(struct npt_ring *ring, size_t offset, uint32_t val)
{
   struct npt_ring_extra *extra = &ring->extra;

   if (unlikely(extra->cached_offset != offset || !extra->cached_data)) {
      const struct npt_region access = NPT_REGION_INIT(offset, sizeof(val));
      if (!npt_region_is_valid(&access) || !npt_region_is_within(&access, &extra->region))
         return false;

      extra->cached_offset = offset;
      extra->cached_data = get_resource_pointer(ring->resource, extra->offset + offset);
   }

   atomic_store_explicit(extra->cached_data, val, memory_order_release);

   return true;
}

/* ====================================================================== */
/* Per-command helpers                                                    */
/* ====================================================================== */

static bool
npt_ring_validate_layout(const struct npt_ring_layout *layout)
{
   const struct npt_resource *res = layout->resource;
   const struct npt_region res_region = { .begin = 0, .end = res->size };

   if (!npt_region_is_valid(&layout->head) ||
       !npt_region_is_valid(&layout->tail) ||
       !npt_region_is_valid(&layout->status) ||
       !npt_region_is_valid(&layout->buffer) ||
       !npt_region_is_valid(&layout->extra))
      return false;

   if (npt_region_size(&layout->head) != sizeof(uint32_t) ||
       npt_region_size(&layout->tail) != sizeof(uint32_t) ||
       npt_region_size(&layout->status) != sizeof(uint32_t))
      return false;

   if (!npt_region_is_aligned(&layout->head, 4) ||
       !npt_region_is_aligned(&layout->tail, 4) ||
       !npt_region_is_aligned(&layout->status, 4))
      return false;

   const size_t buf_size = npt_region_size(&layout->buffer);
   if (!buf_size || !util_is_power_of_two_nonzero(buf_size) ||
       buf_size > NPT_RING_BUFFER_MAX_SIZE)
      return false;

   if (!npt_region_is_within(&layout->head, &res_region) ||
       !npt_region_is_within(&layout->tail, &res_region) ||
       !npt_region_is_within(&layout->status, &res_region) ||
       !npt_region_is_within(&layout->buffer, &res_region) ||
       !npt_region_is_within(&layout->extra, &res_region))
      return false;

   /* Reject overlapping control regions: a buggy guest aliasing head
    * onto tail would corrupt the producer/consumer invariant. */
   if (!npt_region_is_disjoint(&layout->head, &layout->tail) ||
       !npt_region_is_disjoint(&layout->head, &layout->status) ||
       !npt_region_is_disjoint(&layout->tail, &layout->status))
      return false;
   if (!npt_region_is_disjoint(&layout->head, &layout->buffer) ||
       !npt_region_is_disjoint(&layout->tail, &layout->buffer) ||
       !npt_region_is_disjoint(&layout->status, &layout->buffer))
      return false;
   if (!npt_region_is_disjoint(&layout->head, &layout->extra) ||
       !npt_region_is_disjoint(&layout->tail, &layout->extra) ||
       !npt_region_is_disjoint(&layout->status, &layout->extra) ||
       !npt_region_is_disjoint(&layout->buffer, &layout->extra))
      return false;

   return true;
}

struct npt_ring *
npt_ring_find_by_id(struct npt_context *ctx, uint64_t ring_id)
{
   mtx_lock(&ctx->ring_mutex);
   struct npt_ring *ring = NULL;
   list_for_each_entry(struct npt_ring, r, &ctx->rings, head) {
      if (r->id == ring_id) {
         ring = r;
         break;
      }
   }
   mtx_unlock(&ctx->ring_mutex);
   return ring;
}

/* Caller holds ctx->ring_mutex, which is dropped while waiting.  Returns
 * the ring with `ring_id`, waiting for the context path to register it
 * when the guest named it before its CREATE_RING was decoded (a queue's
 * instance ring is created asynchronously and referenced by edges and
 * releases from ring threads that run ahead of the context path).
 * Returns NULL for a ring already destroyed by DESTROY_RING, or once the
 * context is fatal. */
struct npt_ring *
npt_ring_lookup_wait_locked(struct npt_context *ctx, uint64_t ring_id)
{
   const uint64_t report_ns = 5000000000ull;
   uint64_t next_report = npt_ring_now() + report_ns;
   for (;;) {
      list_for_each_entry(struct npt_ring, r, &ctx->rings, head) {
         if (r->id == ring_id)
            return r;
      }
      for (uint32_t i = 0; i < ctx->destroyed_ring_count; i++) {
         if (ctx->destroyed_ring_ids[i] == ring_id)
            return NULL;
      }
      if (ctx->cs_fatal_error)
         return NULL;
      const struct timespec rel = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
#if defined(__APPLE__)
      pthread_cond_timedwait_relative_np(&ctx->ring_cond, &ctx->ring_mutex,
                                         &rel);
#else
      struct timespec ts;
      timespec_get(&ts, TIME_UTC);
      ts.tv_nsec += rel.tv_nsec;
      if (ts.tv_nsec >= 1000000000L) {
         ts.tv_sec += 1;
         ts.tv_nsec -= 1000000000L;
      }
      cnd_timedwait(&ctx->ring_cond, &ctx->ring_mutex, &ts);
#endif
      const uint64_t now = npt_ring_now();
      if (now >= next_report) {
         npt_log("ring %" PRIu64 " named before its CREATE_RING was decoded; "
                 "still waiting", ring_id);
         next_report = now + report_ns;
      }
   }
}

bool
npt_ring_create_from_cmd(struct npt_context *ctx,
                         const struct npt_cmd_create_ring *cmd)
{
   struct npt_resource *res = npt_context_get_resource(ctx, cmd->res_id);
   if (!res || res->fd_type != VIRGL_RESOURCE_FD_SHM) {
      npt_log("create_ring: invalid resource %u", cmd->res_id);
      return false;
   }

   struct npt_ring_layout layout = {
      .resource = res,
      .head = NPT_REGION_INIT(cmd->head_offset, sizeof(uint32_t)),
      .tail = NPT_REGION_INIT(cmd->tail_offset, sizeof(uint32_t)),
      .status = NPT_REGION_INIT(cmd->status_offset, sizeof(uint32_t)),
      .buffer = NPT_REGION_INIT(cmd->buffer_offset, cmd->buffer_size),
      .extra = NPT_REGION_INIT(cmd->extra_offset, cmd->extra_size),
   };

   if (!npt_ring_validate_layout(&layout)) {
      npt_log("create_ring: invalid ring layout");
      return false;
   }

   /* Report the host backend's workaround flags into the guest-visible ring
    * blob; the guest reads this word once at init and gates host-backend-
    * specific shader/cap patches on it. Bounds-checked (4 bytes, 4-aligned,
    * within the resource); the guest picks an offset it keeps disjoint from the
    * ring regions it owns. NPT_WA_FLAGS_PRESENT marks the word as written. */
   if (cmd->workaround_offset) {
      if ((cmd->workaround_offset & 3u) ||
          (size_t)cmd->workaround_offset + sizeof(uint32_t) > res->size) {
         npt_log("create_ring: bad workaround_offset %u", cmd->workaround_offset);
         return false;
      }
      struct npt_d3d_library *lib = npt_renderer_get_library();
      uint32_t flags = (lib ? lib->workaround_flags : 0u) | NPT_WA_FLAGS_PRESENT;
      atomic_store_explicit(
         (_Atomic uint32_t *)((uint8_t *)res->u.data + cmd->workaround_offset),
         flags, memory_order_release);
   }

   struct npt_ring *ring = npt_ring_create(&layout, ctx, cmd->idle_timeout);
   if (!ring) {
      npt_log("create_ring: failed to create ring");
      return false;
   }

   ring->id = cmd->ring_id;

   /* Arm the ring as monitored and stamp a first ALIVE heartbeat before
    * publishing it to ctx->rings, under the same lock the monitor thread
    * scans.  The monitor is the only writer of the ALIVE bit and ticks
    * lazily on monitor_report_period_us, which is slower than the guest's
    * ring watchdog: without a heartbeat stamped before the ring is
    * reachable, the watchdog can observe a created-but-unserviced ring
    * and tear it down. */
   mtx_lock(&ctx->ring_mutex);
   if (cmd->monitor_report_period_us) {
      ring->monitor = true;
      npt_ring_set_status_bits(ring, NPT_RING_STATUS_ALIVE_BIT);
   }
   if (!list_is_empty(&ctx->rings))
      atomic_store_explicit(&ctx->multi_ring, true, memory_order_release);
   list_addtail(&ring->head, &ctx->rings);
   cnd_broadcast(&ctx->ring_cond);
   mtx_unlock(&ctx->ring_mutex);

   if (cmd->monitor_report_period_us &&
       !npt_context_ring_monitor_init(ctx, cmd->monitor_report_period_us)) {
      /* Nothing will refresh that heartbeat without a monitor thread, so
       * fail loudly and let the guest tear down rather than leave it
       * spinning its watchdog to a hard abort.  Unpublish on the way out:
       * the ring reached ctx->rings but never started, and teardown walks
       * that list expecting every entry to own a running thread. */
      npt_log("create_ring: failed to start ring monitor for ring %"
              PRIu64, cmd->ring_id);
      mtx_lock(&ctx->ring_mutex);
      npt_ring_destroy(ring);
      mtx_unlock(&ctx->ring_mutex);
      return false;
   }

   if (cmd->priority_valid) {
      ring->prio_valid = true;
      ring->prio = (int)cmd->priority;
   }

   npt_ring_start(ring);

   npt_log("created ring %" PRIu64 " for context %u (blob res_id=%u)",
           cmd->ring_id, ctx->ctx_id, cmd->res_id);
   return true;
}

bool
npt_ring_destroy_by_id(struct npt_context *ctx, uint64_t ring_id)
{
   struct npt_ring *ring = npt_ring_find_by_id(ctx, ring_id);

   /* Stop without ring_mutex: the thread may need other locks to
    * exit.  npt_ring_stop returns false on self-destruct — fatal. */
   if (!ring || !npt_ring_stop(ring)) {
      npt_log("destroy_ring: ring %" PRIu64 " not found or self-destruct",
              ring_id);
      return false;
   }

   /* Stop broadcast the ring's cond, so peer waiters are on their way
    * out.  Wait for them under ring_mutex -- a waiter pins the ring
    * under it -- so none can arrive between the last check and the
    * destroy. */
   mtx_lock(&ctx->ring_mutex);
   while (atomic_load_explicit(&ring->peer_pins, memory_order_acquire)) {
      mtx_unlock(&ctx->ring_mutex);
      thrd_yield();
      mtx_lock(&ctx->ring_mutex);
   }
   /* Nothing more will ever be decoded here; every deferred release
    * waiting on this ring has what it needs.  Run them outside
    * ring_mutex: a release can reach back into ring-level state. */
   struct list_head settled;
   list_inithead(&settled);
   npt_ring_detach_watches(ring, 0, true, &settled);
   /* Remember the id: a later edge or release naming this ring must not
    * wait for a CREATE_RING that will never come. */
   if (ctx->destroyed_ring_count == ctx->destroyed_ring_cap) {
      const uint32_t cap = ctx->destroyed_ring_cap ? ctx->destroyed_ring_cap * 2 : 16;
      uint64_t *ids = realloc(ctx->destroyed_ring_ids, cap * sizeof(*ids));
      if (ids) {
         ctx->destroyed_ring_ids = ids;
         ctx->destroyed_ring_cap = cap;
      }
   }
   if (ctx->destroyed_ring_count < ctx->destroyed_ring_cap)
      ctx->destroyed_ring_ids[ctx->destroyed_ring_count++] = ring_id;
   npt_ring_destroy(ring);
   cnd_broadcast(&ctx->ring_cond);
   mtx_unlock(&ctx->ring_mutex);
   npt_ring_settle_list(ctx, &settled);
   return true;
}
