/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-ring sync-queue worker thread.  Polls a sync_file fd per
 * submitted fence and retires the fence when the host signals it.
 */

#include "npt_queue.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "npt_context.h"
#include "npt_event.h"
#include "npt_profile.h"
#include "util/u_thread.h"
#include "virgl_fence.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

/* The host D3D library gives the sync path no DEVICE_LOST signal -- only
 * an fd, and for gates a fence value -- so a wall-clock budget is the
 * only way to stop a wedged producer from trapping the guest forever.
 * It has to be wall clock rather than a count of poll returns: the
 * ring's gate event is shared by all of that ring's gates, so another
 * gate's fire wakes this poll arbitrarily often without any time
 * passing.  Healthy work finishes orders of magnitude inside it. */
#define NPT_QUEUE_DEVICE_LOST_SEC 30u

/* ----------------------------------------------------------------- */
/* sync alloc / free / retire                                          */
/* ----------------------------------------------------------------- */

/* Take every queued wakeup off the fd.  The event is level-triggered and
 * shared across the ring's gates, so a token left behind would wake the
 * next gate on a fire that was never meant for it.  The wait fd is
 * non-blocking (npt_event.c), so the loop ends on EAGAIN.  Returns
 * whether anything was actually there: a poll that reports readable and
 * then yields nothing is a dead fd (POLLERR/POLLHUP), not a fire. */
static bool
npt_queue_drain_wakeups(int fd)
{
   if (fd < 0)
      return false;

   bool drained = false;
   uint64_t token;
   ssize_t n;
   do {
      n = read(fd, &token, sizeof(token));
      drained |= n > 0;
   } while (n > 0 || (n < 0 && errno == EINTR));

   return drained;
}

static struct npt_queue_sync *
npt_queue_alloc_sync(uint32_t ring_idx,
                     uint64_t fence_id,
                     const struct npt_event_paired *paired)
{
   struct npt_queue_sync *sync = malloc(sizeof(*sync));
   if (!sync)
      return NULL;

   sync->sync_fd  = paired->fd;
   sync->ring_idx = ring_idx;
   sync->fence_id = fence_id;
   sync->release_proxy = paired->release_proxy;
   sync->check_fence = paired->check_fence;
   sync->check_value = paired->check_value;
   sync->fast_poll = false;
   sync->deadline_ns = npt_profile_now_ns() +
      (uint64_t)NPT_QUEUE_DEVICE_LOST_SEC * 1000000000ull;

   return sync;
}

static void
npt_queue_free_sync(struct npt_queue_sync *sync)
{
   if (sync->sync_fd >= 0)
      close(sync->sync_fd);
   free(sync);
}

static inline void
npt_queue_sync_retire(struct npt_queue *queue, struct npt_queue_sync *sync)
{
   queue->context->retire_fence(queue->context->ctx_id,
                                 sync->ring_idx, sync->fence_id);
   /* Belt-and-braces against stranded virgl fence-table entries: the
    * synchronous submit path's take_fd normally empties the table, but
    * any entry left behind (e.g. a take that lost a race with a very
    * fast completion) would otherwise be swept with a syscall by every
    * later virgl_fence_set_fd, forever.  A no-op on the common path. */
   virgl_fence_retire(virgl_fence_ring_key(sync->ring_idx, sync->fence_id));
   /* AUTO_RELEASE arm: the proxy reference was transferred to this
    * entry at pairing; the fence has retired (the proxy fired), so the
    * D3D library is done writing the signal handle -- release it. */
   npt_event_release_proxy(queue->context, sync->release_proxy);
   if (sync->check_fence)
      npt_d3d12_gate_release(sync->check_fence);
   npt_queue_free_sync(sync);
}

/* ----------------------------------------------------------------- */
/* submit                                                              */
/* ----------------------------------------------------------------- */

bool
npt_queue_sync_submit(struct npt_queue *queue,
                      uint32_t ring_idx,
                      uint64_t fence_id,
                      const struct npt_event_paired *paired)
{
   struct npt_queue_sync *sync =
      npt_queue_alloc_sync(ring_idx, fence_id, paired);
   if (!sync) {
      if (paired->fd >= 0)
         close(paired->fd);
      npt_event_release_proxy(queue->context, paired->release_proxy);
      if (paired->check_fence)
         npt_d3d12_gate_release(paired->check_fence);
      return false;
   }

   /* Append to the queue's sync list and wake the worker. */
   mtx_lock(&queue->sync_thread.mutex);
   list_addtail(&sync->head, &queue->sync_thread.syncs);
   cnd_signal(&queue->sync_thread.cond);
   mtx_unlock(&queue->sync_thread.mutex);

   return true;
}

/* ----------------------------------------------------------------- */
/* worker thread                                                       */
/* ----------------------------------------------------------------- */

static int
npt_wait_sync_fd(int fd, int timeout_ms)
{
   if (fd < 0)
      return 0;

   struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
   int ret;
   do {
      ret = poll(&pfd, 1, timeout_ms);
   } while (ret < 0 && errno == EINTR);

   /* Treat POLLNVAL / POLLERR / POLLHUP and any other error as a
    * signal so the worker retires the fence rather than re-polling a
    * dead fd forever.  A concurrent close() of the sync fd can race a
    * poll already in flight; the sync queue is that fd's only
    * downstream and must not wedge. */
   if (ret > 0 && (pfd.revents & (POLLNVAL | POLLERR | POLLHUP)))
      return 1;
   if (ret < 0)
      return 1;

   return ret;
}

static int
npt_queue_thread(void *arg)
{
   struct npt_queue *queue = arg;
   struct npt_context *ctx = queue->context;
   char thread_name[16];
   snprintf(thread_name, sizeof(thread_name), "npt-queue-%u", ctx->ctx_id);
   u_thread_setname(thread_name);

#if defined(__APPLE__)
   /* Per-thread QoS (see npt_ring_thread for why nice is not usable on
    * Darwin).  This thread turns GPU completion into the guest's fence
    * wake, so its scheduling delay lands directly in every waiter's
    * latency. */
   pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

   /* sync_thread.mutex protects the list; dropped across the poll,
    * reacquired before popping the entry. */
   const int kPollTimeoutMs = 3000;

   /* Value-gated (GATE_WAIT) syncs poll on a short period.  The wakeup
    * event is shared across the ring's gates, so this gate's fire can be
    * legally consumed by another gate's drain, leaving the value complete
    * with the event silent until the next timeout notices.  250 ms bounds
    * that latency at negligible idle cost, and the common path never
    * waits it out.  A gate that has already seen such a wake drops to
    * kGateFastPollMs. */
   const int kGatePollTimeoutMs = 250;
   const int kGateFastPollMs = 2;

   mtx_lock(&queue->sync_thread.mutex);
   while (true) {
      while (list_is_empty(&queue->sync_thread.syncs) && !queue->sync_thread.join)
         cnd_wait(&queue->sync_thread.cond, &queue->sync_thread.mutex);

      if (queue->sync_thread.join)
         break;

      struct npt_queue_sync *sync =
         list_first_entry(&queue->sync_thread.syncs, struct npt_queue_sync, head);

      mtx_unlock(&queue->sync_thread.mutex);

      struct timespec t0, t1;
      const bool trace = NPT_DEBUG(FENCE_TRACE);
      if (trace) {
         clock_gettime(CLOCK_MONOTONIC, &t0);
         t1 = t0;
      }
      int rc = 0;

      if (sync->check_fence) {
         /* The event is only a wakeup source; the value is the truth.
          * Check it before waiting, since this gate's fire may already
          * have been consumed and polling first would then wait out a
          * full period while the value sits complete. */
         if (npt_d3d12_gate_reached(sync->check_fence, sync->check_value)) {
            npt_queue_drain_wakeups(sync->sync_fd);
            mtx_lock(&queue->sync_thread.mutex);
            goto retire;
         }

         rc = npt_wait_sync_fd(sync->sync_fd,
                               sync->fast_poll ? kGateFastPollMs
                                               : kGatePollTimeoutMs);

         bool stale = false;
         if (rc > 0 && npt_queue_drain_wakeups(sync->sync_fd)) {
            stale = !npt_d3d12_gate_reached(sync->check_fence,
                                            sync->check_value);
         }

         mtx_lock(&queue->sync_thread.mutex);
         if (stale) {
            /* A fire arrived but the value is not ours: this gate's own
             * fire may already have been drained by an earlier one and
             * will never wake us. */
            sync->fast_poll = true;
         }
         if (npt_profile_now_ns() < sync->deadline_ns)
            continue;
         npt_log("queue %u fence_id=%" PRIu64 ": gate value %" PRIu64
                 " not reached in %us, retiring as device-lost",
                 queue->ring_idx, sync->fence_id, sync->check_value,
                 NPT_QUEUE_DEVICE_LOST_SEC);
         goto retire;
      }

      rc = npt_wait_sync_fd(sync->sync_fd, kPollTimeoutMs);
      if (trace)
         clock_gettime(CLOCK_MONOTONIC, &t1);

      /* Consume the fire's token(s) like the gate path does: the proxy
       * event is manual-reset and nothing else ever drains its exported
       * pipe, so a leftover token would make the NEXT arm of this
       * guest HANDLE (guests reuse them) start out readable and retire
       * its fence before the GPU work ran. */
      if (rc > 0)
         npt_queue_drain_wakeups(sync->sync_fd);

      mtx_lock(&queue->sync_thread.mutex);

      if (rc == 0) {
         /* Poll timeout: keep waiting until the device-lost budget is
          * spent, then retire anyway so a wedged host (driver hang,
          * missing sync_file signal) doesn't trap the guest forever. */
         if (npt_profile_now_ns() < sync->deadline_ns)
            continue;
         npt_log("queue %u fence_id=%" PRIu64 ": no completion in %us, "
                 "retiring as device-lost", queue->ring_idx, sync->fence_id,
                 NPT_QUEUE_DEVICE_LOST_SEC);
      }
retire:;

      if (trace) {
         const int64_t dt_ns =
            (int64_t)(t1.tv_sec  - t0.tv_sec ) * 1000000000ll +
            (int64_t)(t1.tv_nsec - t0.tv_nsec);
         const uint64_t wait_us = dt_ns > 0 ? (uint64_t)(dt_ns / 1000) : 0;
         npt_profile_log_q_retire(queue->ring_idx, sync->fence_id,
                                  sync->sync_fd, wait_us, rc);
      }

      list_del(&sync->head);
      npt_queue_sync_retire(queue, sync);
   }
   mtx_unlock(&queue->sync_thread.mutex);

   return 0;
}

/* ----------------------------------------------------------------- */
/* thread init / fini                                                  */
/* ----------------------------------------------------------------- */

static int
npt_queue_sync_thread_init(struct npt_queue *queue)
{
   STATIC_ASSERT(thrd_success == 0);

   int ret = mtx_init(&queue->sync_thread.mutex, mtx_plain);
   if (ret != thrd_success)
      return ret;

   ret = cnd_init(&queue->sync_thread.cond);
   if (ret != thrd_success)
      goto fail_cnd_init;

   list_inithead(&queue->sync_thread.syncs);
   queue->sync_thread.join = false;

   ret = thrd_create(&queue->sync_thread.thread, npt_queue_thread, queue);
   if (ret != thrd_success)
      goto fail_thrd_create;

   return 0;

fail_thrd_create:
   cnd_destroy(&queue->sync_thread.cond);
fail_cnd_init:
   mtx_destroy(&queue->sync_thread.mutex);
   return ret;
}

static void
npt_queue_sync_thread_fini(struct npt_queue *queue)
{
   /* Signal exit and join. */
   mtx_lock(&queue->sync_thread.mutex);
   queue->sync_thread.join = true;
   cnd_signal(&queue->sync_thread.cond);
   mtx_unlock(&queue->sync_thread.mutex);

   thrd_join(queue->sync_thread.thread, NULL);

   /* Drain leftover syncs: free them and let the renderer treat
    * unretired fences as context-destroy. */
   unsigned leftover = 0;
   list_for_each_entry_safe (struct npt_queue_sync, sync,
                             &queue->sync_thread.syncs, head) {
      list_del(&sync->head);
      npt_queue_sync_retire(queue, sync);
      leftover++;
   }
   if (leftover)
      npt_log("queue %u: drained %u leftover sync(s) on destroy",
              queue->ring_idx, leftover);

   mtx_destroy(&queue->sync_thread.mutex);
   cnd_destroy(&queue->sync_thread.cond);
}

/* ----------------------------------------------------------------- */
/* create / destroy                                                    */
/* ----------------------------------------------------------------- */

struct npt_queue *
npt_queue_create(struct npt_context *ctx, uint32_t ring_idx)
{
   /* Reject ring_idx values that would overflow ctx->sync_queues[].
    * Submit already bounds-checks, but creation has other callers; a
    * defensive check here prevents an orphan queue from arising on a
    * corrupt-command path. */
   if (ring_idx >= ARRAY_SIZE(ctx->sync_queues))
      return NULL;

   struct npt_queue *queue = calloc(1, sizeof(*queue));
   if (!queue)
      return NULL;

   queue->context  = ctx;
   queue->ring_idx = ring_idx;

   if (npt_queue_sync_thread_init(queue)) {
      free(queue);
      return NULL;
   }

   return queue;
}

void
npt_queue_destroy(struct npt_queue *queue)
{
   if (!queue)
      return;
   npt_queue_sync_thread_fini(queue);
   free(queue);
}
