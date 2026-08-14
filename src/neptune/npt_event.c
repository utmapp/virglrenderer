/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_event.h"

#include "npt_context.h"
#include "npt_library.h"
#include "npt_renderer.h"
#include "npt_transport_defs.h"

#include "util/hash_table.h"
#include "util/list.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif

static int
event_fd_create(struct npt_event_fd *out)
{
#if defined(__linux__)
   out->fd = eventfd(0, EFD_CLOEXEC);
   return out->fd < 0 ? -1 : 0;
#elif defined(__APPLE__)
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_event_create) {
      npt_log("event: backend event API unavailable");
      return -1;
   }
   /* Manual-reset matches Win32's default for guest-created HANDLEs
    * (we don't auto-clear on read). */
   out->handle = lib->pfn_event_create(/*manual_reset=*/1, /*initial_state=*/0);
   return out->handle ? 0 : -1;
#else
   /* Prefer pipe2(O_CLOEXEC) where available (Linux, FreeBSD) to
    * avoid the fd-leak window between pipe() and fcntl().  Fall back
    * to pipe() + fcntl() on platforms without pipe2. */
   int fds[2];
#ifdef HAVE_PIPE2
   if (pipe2(fds, O_CLOEXEC) == 0) {
      out->read_fd = fds[0];
      out->write_fd = fds[1];
      return 0;
   }
   if (errno != ENOSYS)
      return -1;
#endif
   if (pipe(fds) != 0)
      return -1;
   /* Best-effort CLOEXEC: non-fatal, fds remain usable on failure. */
   for (int i = 0; i < 2; i++) {
      int flags = fcntl(fds[i], F_GETFD);
      if (flags >= 0)
         fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC);
   }
   out->read_fd = fds[0];
   out->write_fd = fds[1];
   return 0;
#endif
}

static void
event_fd_destroy(struct npt_event_fd *fd)
{
#if defined(__linux__)
   if (fd->fd >= 0) {
      close(fd->fd);
      fd->fd = -1;
   }
#elif defined(__APPLE__)
   if (fd->handle) {
      struct npt_d3d_library *lib = npt_renderer_get_library();
      if (lib && lib->pfn_event_close)
         lib->pfn_event_close(fd->handle);
      fd->handle = NULL;
   }
#else
   if (fd->read_fd >= 0) {
      close(fd->read_fd);
      fd->read_fd = -1;
   }
   if (fd->write_fd >= 0) {
      close(fd->write_fd);
      fd->write_fd = -1;
   }
#endif
}

/* The value handed to the host as the HANDLE; SetEvent acts on it.
 * An fd cast to a pointer on eventfd/pipe platforms, the backend's
 * event handle on darwin. */
static void *
event_fd_signal_handle(const struct npt_event_fd *fd)
{
#if defined(__linux__)
   return fd->fd >= 0 ? (void *)(uintptr_t)fd->fd : NULL;
#elif defined(__APPLE__)
   return fd->handle;
#else
   return fd->write_fd >= 0 ? (void *)(uintptr_t)fd->write_fd : NULL;
#endif
}

/* Caller polls + closes the returned dup.
 *
 * Always non-blocking: a ring's gate event is shared by every gate on
 * it, so the sync worker cannot know how many wakeups are queued and
 * has to drain with a read loop that ends on EAGAIN.  A blocking fd
 * would park that worker in read() with the whole queue behind it. */
static int
event_fd_dup_wait_fd(const struct npt_event_fd *fd)
{
#if defined(__linux__)
   int wait_fd = dup(fd->fd);
#elif defined(__APPLE__)
   struct npt_d3d_library *lib = npt_renderer_get_library();
   int wait_fd = (lib && lib->pfn_event_dup_fd)
                    ? lib->pfn_event_dup_fd(fd->handle) : -1;
#else
   int wait_fd = dup(fd->read_fd);
#endif
   if (wait_fd < 0)
      return -1;

   const int flags = fcntl(wait_fd, F_GETFL);
   if (flags < 0 || fcntl(wait_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      npt_log("event: O_NONBLOCK on the wait fd failed: %s", strerror(errno));
      close(wait_fd);
      return -1;
   }
   return wait_fd;
}

static void
event_fd_init_invalid(struct npt_event_fd *fd)
{
#if defined(__linux__)
   fd->fd = -1;
#elif defined(__APPLE__)
   fd->handle = NULL;
#else
   fd->read_fd = -1;
   fd->write_fd = -1;
#endif
}

static bool
event_fd_is_valid(const struct npt_event_fd *fd)
{
#if defined(__linux__)
   return fd->fd >= 0;
#elif defined(__APPLE__)
   return fd->handle != NULL;
#else
   return fd->read_fd >= 0;
#endif
}

static uint32_t
tok_hash(const void *key)
{
   uint64_t t = *(const uint64_t *)key;
   t ^= t >> 33; t *= 0xff51afd7ed558ccdULL;
   t ^= t >> 33; t *= 0xc4ceb9fe1a85ec53ULL;
   t ^= t >> 33;
   return (uint32_t)t;
}

static bool
tok_equal(const void *a, const void *b)
{
   return *(const uint64_t *)a == *(const uint64_t *)b;
}

bool
npt_event_init(struct npt_context *ctx)
{
   if (mtx_init(&ctx->event_mutex, mtx_plain) != thrd_success)
      return false;

   ctx->event_proxies = _mesa_hash_table_create(NULL, tok_hash, tok_equal);
   if (!ctx->event_proxies) {
      mtx_destroy(&ctx->event_mutex);
      return false;
   }

   list_inithead(&ctx->event_pending_arms);
   list_inithead(&ctx->event_pending_fences);
   for (unsigned i = 0; i < ARRAY_SIZE(ctx->gate_ring); i++)
      event_fd_init_invalid(&ctx->gate_ring[i]);
   return true;
}

void
npt_event_fini(struct npt_context *ctx)
{
   if (!ctx->event_proxies)
      return;

   mtx_lock(&ctx->event_mutex);

   /* Guests should send RELEASE_EVENT for every REGISTER, leaving
    * the table empty by teardown.  Log non-zero residue so the leak
    * is visible. */
   uint32_t leaked = _mesa_hash_table_num_entries(ctx->event_proxies);
   if (leaked)
      npt_log("event_fini: %u proxies leaked at context teardown", leaked);

   /* There is no way to retract a SetEventOnCompletion, so a gate event
    * must not be destroyed while a registration on it can still fire:
    * the later signal would hit a recycled fd number, or on darwin a
    * freed handle.  An unpaired gate arm whose value is demonstrably not
    * reached is exactly that case, so it pins its ring's event for the
    * life of the process.  Only a guest that dies mid-gate gets here,
    * and at most one event per ring index is ever pinned. */
   bool gate_pinned[ARRAY_SIZE(ctx->gate_ring)] = { false };

   list_for_each_entry_safe(struct npt_event_pending_arm, p,
                            &ctx->event_pending_arms, head) {
      if (p->check_fence) {
         if (p->ring_idx < ARRAY_SIZE(gate_pinned) &&
             !npt_d3d12_gate_reached(p->check_fence, p->check_value))
            gate_pinned[p->ring_idx] = true;
         npt_d3d12_gate_release(p->check_fence);
      }
      if (p->dup_fd >= 0)
         close(p->dup_fd);
      list_del(&p->head);
      free(p);
   }

   list_for_each_entry_safe(struct npt_event_pending_fence, f,
                            &ctx->event_pending_fences, head) {
      list_del(&f->head);
      free(f);
   }

   hash_table_foreach(ctx->event_proxies, entry) {
      struct npt_event_proxy *pr = entry->data;
      event_fd_destroy(&pr->proxy);
      free(pr);
   }
   _mesa_hash_table_destroy(ctx->event_proxies, NULL);
   ctx->event_proxies = NULL;

   for (unsigned i = 0; i < ARRAY_SIZE(ctx->gate_ring); i++) {
      if (!event_fd_is_valid(&ctx->gate_ring[i]))
         continue;
      if (gate_pinned[i])
         npt_log("event_fini: ring %u gate event kept open; a fence "
                 "registration on it can still fire", i);
      else
         event_fd_destroy(&ctx->gate_ring[i]);
      event_fd_init_invalid(&ctx->gate_ring[i]);
   }

   mtx_unlock(&ctx->event_mutex);
   mtx_destroy(&ctx->event_mutex);
}

static struct npt_event_proxy *
lookup_locked(struct npt_context *ctx, uint64_t token)
{
   struct hash_entry *e =
      _mesa_hash_table_search_pre_hashed(ctx->event_proxies,
                                         tok_hash(&token), &token);
   return e ? e->data : NULL;
}

static void
npt_event_proxy_unref_locked(struct npt_context *ctx,
                             struct npt_event_proxy *pr,
                             struct hash_entry *e);

/* Create + insert a proxy at `initial_refcount`.  Caller holds event_mutex.
 * REGISTER_EVENT passes 1 (the registration reference).  A lazy create from
 * npt_event_arm passes 0 so ONLY the arm's reference holds the proxy: ARM and
 * its REGISTER_EVENT travel on different host channels (a per-event ring vs
 * the renderer command stream) and can be decoded out of order.  If ARM lands
 * first and took a registration reference too, the later REGISTER_EVENT would
 * add a second one that the guest's single RELEASE_EVENT never balances,
 * leaking the proxy.  With 0 here, REGISTER always contributes exactly one
 * registration reference regardless of arrival order. */
static struct npt_event_proxy *
event_proxy_create_locked(struct npt_context *ctx, uint64_t token,
                          uint32_t initial_refcount)
{
   struct npt_event_fd hfd;
   event_fd_init_invalid(&hfd);
   if (event_fd_create(&hfd) < 0) {
      npt_log("event: fd create failed: %s", strerror(errno));
      return NULL;
   }
   struct npt_event_proxy *pr = calloc(1, sizeof(*pr));
   if (!pr) {
      event_fd_destroy(&hfd);
      return NULL;
   }
   pr->token    = token;
   pr->proxy    = hfd;
   pr->refcount = initial_refcount;
   /* Key points into pr->token so the hash key stays valid for pr's
    * lifetime. */
   _mesa_hash_table_insert_pre_hashed(ctx->event_proxies,
                                      tok_hash(&pr->token),
                                      &pr->token, pr);
   return pr;
}

void
npt_event_register(struct npt_context *ctx, uint64_t token)
{
   if (!token)
      return;

   mtx_lock(&ctx->event_mutex);
   struct npt_event_proxy *pr = lookup_locked(ctx, token);
   if (pr) {
      pr->refcount++;
      mtx_unlock(&ctx->event_mutex);
      return;
   }
   event_proxy_create_locked(ctx, token, /*initial_refcount=*/1);
   mtx_unlock(&ctx->event_mutex);
}

/* Caller holds event_mutex.  Unlinks the fence parked on ring_idx and
 * transfers it to the caller. */
static struct npt_event_pending_fence *
event_take_parked_locked(struct npt_context *ctx, uint32_t ring_idx)
{
   list_for_each_entry(struct npt_event_pending_fence, f,
                       &ctx->event_pending_fences, head) {
      if (f->ring_idx == ring_idx) {
         list_del(&f->head);
         return f;
      }
   }
   return NULL;
}

/* Hand a parked fence to the sync queue and free it.  Caller must have
 * dropped event_mutex: pairing reaches back into the renderer. */
static void
event_pair_parked(struct npt_context *ctx,
                  struct npt_event_pending_fence *parked,
                  const struct npt_event_paired *paired)
{
   const uint32_t ring_idx = parked->ring_idx;
   const uint64_t fence_id = parked->fence_id;
   free(parked);

   if (!npt_context_pair_event_fence(ctx, ring_idx, fence_id, paired,
                                     /*register_fd=*/false))
      npt_log("event: pairing parked fence (ring=%u id=%" PRIu64 ") failed",
              ring_idx, fence_id);
}

bool
npt_event_arm(struct npt_context *ctx, uint64_t token, uint32_t ring_idx,
              uint32_t arm_flags)
{
   if (!token)
      return false;

   const bool auto_release = (arm_flags & NPT_EVENT_ARM_FLAG_AUTO_RELEASE) != 0;

   mtx_lock(&ctx->event_mutex);
   struct npt_event_proxy *pr =
      auto_release ? NULL : lookup_locked(ctx, token);
   if (!pr) {
      /* Lazy-create for an ARM that beat its REGISTER_EVENT, WITHOUT a
       * registration reference — the arm reference taken below holds it.
       *
       * AUTO_RELEASE arms never send REGISTER at all: the arm reference
       * is the proxy's single reference and each arm gets a fresh proxy,
       * never a table hit.  The guest reuses one HANDLE (= token) across
       * waits, and a prior arm's proxy for it can still be alive here --
       * already signaled, since nothing clears the host event -- so
       * reusing it would retire the new fence before the GPU work, and
       * that spurious retire drops the last reference while the decode
       * thread may still be inside the D3D call registering the handle.
       * The insert replaces the token's table entry, so the SEOC that
       * follows this ARM in stream order resolves to this proxy; the
       * displaced proxy lives on through its outstanding references and
       * is released by pointer. */
      pr = event_proxy_create_locked(ctx, token, /*initial_refcount=*/0);
      if (!pr) {
         mtx_unlock(&ctx->event_mutex);
         return false;
      }
   }
   /* Take the arm reference now so the fd stays alive even if the guest sends
    * RELEASE_EVENT before the fence completes, and so the failure paths below
    * can undo it (freeing a just-lazy-created proxy) via the unref helper. */
   pr->refcount++;

   int dup_fd = event_fd_dup_wait_fd(&pr->proxy);
   if (dup_fd < 0) {
      npt_log("event: dup(proxy wait fd) failed: %s", strerror(errno));
      npt_event_proxy_unref_locked(ctx, pr, NULL);
      mtx_unlock(&ctx->event_mutex);
      return false;
   }

   /* A fence for this ring may already be parked, having outrun this ARM
    * on the independent virtio channel.  Consume the arm here exactly as
    * a pop would. */
   struct npt_event_pending_fence *parked =
      event_take_parked_locked(ctx, ring_idx);
   if (parked) {
      /* AUTO_RELEASE: transfer the arm reference to the sync-queue entry
       * instead of unreffing, so the signal handle the D3D library stored
       * stays valid until it has been written. */
      if (!auto_release)
         npt_event_proxy_unref_locked(ctx, pr, NULL);
      mtx_unlock(&ctx->event_mutex);

      const struct npt_event_paired paired = {
         .fd = dup_fd,
         .release_proxy = auto_release ? pr : NULL,
      };
      event_pair_parked(ctx, parked, &paired);
      return true;
   }

   struct npt_event_pending_arm *p = calloc(1, sizeof(*p));
   if (!p) {
      close(dup_fd);
      npt_event_proxy_unref_locked(ctx, pr, NULL);
      mtx_unlock(&ctx->event_mutex);
      return false;
   }
   p->ring_idx = ring_idx;
   p->dup_fd   = dup_fd;
   p->proxy    = pr;
   p->auto_release = auto_release;
   list_addtail(&p->head, &ctx->event_pending_arms);

   mtx_unlock(&ctx->event_mutex);
   return true;
}

/* Caller holds ctx->event_mutex.  Passing `e` from an existing
 * lookup avoids a re-search; NULL forces a fresh lookup. */
static void
npt_event_proxy_unref_locked(struct npt_context *ctx,
                              struct npt_event_proxy *pr,
                              struct hash_entry *e)
{
   if (--pr->refcount > 0)
      return;

   event_fd_destroy(&pr->proxy);

   if (!e) {
      e = _mesa_hash_table_search_pre_hashed(ctx->event_proxies,
                                             tok_hash(&pr->token),
                                             &pr->token);
   }
   /* An AUTO_RELEASE arm for the same token may have replaced this
    * proxy's table entry with a fresh one; only remove the entry if it
    * is still ours. */
   if (e && e->data == pr)
      _mesa_hash_table_remove(ctx->event_proxies, e);
   free(pr);
}

/* GATE_WAIT: D3D12 monitored-fence gate arm with value-checked
 * retirement (see npt_cmd_gate_wait). */
bool
npt_event_gate_wait(struct npt_context *ctx, void *fence, uint64_t value,
                    uint32_t ring_idx)
{
   if (!fence || ring_idx >= ARRAY_SIZE(ctx->gate_ring))
      return false;

   mtx_lock(&ctx->event_mutex);
   struct npt_event_fd *gate = &ctx->gate_ring[ring_idx];
   if (!event_fd_is_valid(gate) && event_fd_create(gate) < 0) {
      event_fd_init_invalid(gate);
      mtx_unlock(&ctx->event_mutex);
      npt_log("gate_wait: gate event create failed: %s", strerror(errno));
      return false;
   }

   int dup_fd = event_fd_dup_wait_fd(gate);
   if (dup_fd < 0) {
      mtx_unlock(&ctx->event_mutex);
      npt_log("gate_wait: dup(gate wait fd) failed: %s", strerror(errno));
      return false;
   }

   /* Register the wakeup with the D3D library BEFORE any pairing so an
    * instant fire is observable on the first poll.  The library only
    * borrows the signal handle, which the per-ring gate keeps alive. */
   npt_d3d12_gate_addref(fence);
   if (!npt_d3d12_gate_seoc(fence, value, event_fd_signal_handle(gate))) {
      npt_d3d12_gate_release(fence);
      close(dup_fd);
      mtx_unlock(&ctx->event_mutex);
      npt_log("gate_wait: SetEventOnCompletion failed (v=%" PRIu64 ")",
              value);
      return false;
   }

   struct npt_event_pending_fence *parked =
      event_take_parked_locked(ctx, ring_idx);
   if (parked) {
      mtx_unlock(&ctx->event_mutex);

      const struct npt_event_paired paired = {
         .fd = dup_fd,
         .check_fence = fence,
         .check_value = value,
      };
      event_pair_parked(ctx, parked, &paired);
      return true;
   }

   struct npt_event_pending_arm *p = calloc(1, sizeof(*p));
   if (!p) {
      npt_d3d12_gate_release(fence);
      close(dup_fd);
      mtx_unlock(&ctx->event_mutex);
      return false;
   }
   p->ring_idx = ring_idx;
   p->dup_fd = dup_fd;
   p->proxy = NULL;
   p->check_fence = fence;
   p->check_value = value;
   list_addtail(&p->head, &ctx->event_pending_arms);

   mtx_unlock(&ctx->event_mutex);
   return true;
}

void
npt_event_release_proxy(struct npt_context *ctx, struct npt_event_proxy *pr)
{
   if (!pr)
      return;

   mtx_lock(&ctx->event_mutex);
   npt_event_proxy_unref_locked(ctx, pr, NULL);
   mtx_unlock(&ctx->event_mutex);
}

void
npt_event_release(struct npt_context *ctx, uint64_t token)
{
   if (!token)
      return;

   mtx_lock(&ctx->event_mutex);
   struct hash_entry *e =
      _mesa_hash_table_search_pre_hashed(ctx->event_proxies,
                                         tok_hash(&token), &token);
   if (!e) {
      mtx_unlock(&ctx->event_mutex);
      return;
   }
   npt_event_proxy_unref_locked(ctx, e->data, e);
   mtx_unlock(&ctx->event_mutex);
}

int
npt_event_pop_arm_or_park_fence(struct npt_context *ctx, uint32_t ring_idx,
                                uint64_t fence_id,
                                struct npt_event_paired *out)
{
   /* Atomic pop-or-park under event_mutex: either the ARM is already
    * decoded (return its proxy fd) or the fence parks until the ARM
    * lands (npt_event_arm pairs it).  The fence and the ARM travel on
    * independent channels on the Windows path (virtio ctrl queue vs
    * npt event ring), so either order is legal. */
   int fd = -1;
   memset(out, 0, sizeof(*out));
   out->fd = -1;
   mtx_lock(&ctx->event_mutex);
   list_for_each_entry_safe(struct npt_event_pending_arm, p,
                            &ctx->event_pending_arms, head) {
      if (p->ring_idx == ring_idx) {
         fd = p->dup_fd;
         out->fd = fd;
         out->check_fence = p->check_fence;
         out->check_value = p->check_value;
         list_del(&p->head);
         if (p->proxy) {
            /* AUTO_RELEASE: transfer the arm reference to the caller's
             * sync-queue entry (released after retirement, post-fire). */
            if (p->auto_release)
               out->release_proxy = p->proxy;
            else
               npt_event_proxy_unref_locked(ctx, p->proxy, NULL);
         }
         free(p);
         break;
      }
   }
   if (fd < 0) {
      struct npt_event_pending_fence *f = calloc(1, sizeof(*f));
      if (!f) {
         mtx_unlock(&ctx->event_mutex);
         return NPT_EVENT_FENCE_ERR;
      }
      f->ring_idx = ring_idx;
      f->fence_id = fence_id;
      list_addtail(&f->head, &ctx->event_pending_fences);
      static int parked_logged;
      if (parked_logged < 8) {
         parked_logged++;
         npt_log("event: fence (ring=%u id=%" PRIu64 ") outran its ARM; "
                 "parked until the ARM decodes", ring_idx, fence_id);
      }
      mtx_unlock(&ctx->event_mutex);
      return NPT_EVENT_FENCE_PARKED;
   }
   mtx_unlock(&ctx->event_mutex);
   return fd;
}

/* Retire every parked fence matching `ring_idx`, or all of them when
 * `all`.  Collected under the mutex and retired after it is dropped:
 * retire_fence reaches back into the renderer and must not run with an
 * npt lock held. */
static void
npt_event_drain_parked(struct npt_context *ctx, uint32_t ring_idx, bool all)
{
   struct list_head doomed;
   list_inithead(&doomed);

   mtx_lock(&ctx->event_mutex);
   list_for_each_entry_safe(struct npt_event_pending_fence, f,
                            &ctx->event_pending_fences, head) {
      if (all || f->ring_idx == ring_idx) {
         list_del(&f->head);
         list_addtail(&f->head, &doomed);
      }
   }
   mtx_unlock(&ctx->event_mutex);

   list_for_each_entry_safe(struct npt_event_pending_fence, f, &doomed,
                            head) {
      ctx->retire_fence(ctx->ctx_id, f->ring_idx, f->fence_id);
      list_del(&f->head);
      free(f);
   }
}

void
npt_event_drain_parked_fences(struct npt_context *ctx)
{
   npt_event_drain_parked(ctx, 0, /*all=*/true);
}

void
npt_event_drain_parked_ring(struct npt_context *ctx, uint32_t ring_idx)
{
   npt_event_drain_parked(ctx, ring_idx, /*all=*/false);
}

/* Decode-time pins.  A substituted handle is used by the D3D call for
 * the rest of the dispatched command, but the proxy's other references
 * can all drop concurrently: the guest waiter's RELEASE_EVENT may
 * interleave between the app's ARM and the method using the token
 * (separate guest threads share one method ring), after which the arm
 * reference is the last one -- and the out-of-band fence thread drops
 * THAT one the moment the fence pops the arm.  Pin every proxy a
 * command's decode resolves until the command's dispatch returns.
 * Substitution and dispatch run on the same thread, so the pin list is
 * thread-local; npt_context_dispatch_one_command drains it. */
#define NPT_EVENT_MAX_PINS_PER_CMD 8
static _Thread_local struct {
   struct npt_event_proxy *pr[NPT_EVENT_MAX_PINS_PER_CMD];
   unsigned count;
} event_cmd_pins;

void
npt_event_unpin_dispatched(struct npt_context *ctx)
{
   if (!event_cmd_pins.count)
      return;

   mtx_lock(&ctx->event_mutex);
   for (unsigned i = 0; i < event_cmd_pins.count; i++)
      npt_event_proxy_unref_locked(ctx, event_cmd_pins.pr[i], NULL);
   mtx_unlock(&ctx->event_mutex);
   event_cmd_pins.count = 0;
}

/* Out-of-line so npt_cs.h doesn't need npt_context.h (header cycle). */
void *
npt_event_replace_by_token(struct npt_dispatch_context *dispatch,
                            npt_object_id id)
{
   if (!id || !dispatch)
      return NULL;

   struct npt_context *ctx = npt_context_from_dispatch(dispatch);
   const uint64_t token = (uint64_t)id;
   if (!ctx || !ctx->event_proxies)
      return NULL;

   mtx_lock(&ctx->event_mutex);
   struct npt_event_proxy *pr = lookup_locked(ctx, token);
   void *ret = pr ? event_fd_signal_handle(&pr->proxy) : NULL;
   if (pr) {
      if (event_cmd_pins.count < NPT_EVENT_MAX_PINS_PER_CMD) {
         pr->refcount++;
         event_cmd_pins.pr[event_cmd_pins.count++] = pr;
      } else {
         /* No command carries this many event args; keep the handle
          * usable but loudly unpinned rather than corrupt the list. */
         npt_log("event: pin list overflow for token %" PRIu64, token);
      }
   }
   mtx_unlock(&ctx->event_mutex);
   return ret;
}
