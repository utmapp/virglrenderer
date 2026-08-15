/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_context.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

#include "npt_com.h"
#include "npt_dispatch.h"
#include "npt_event.h"
#include "npt_feedback.h"
#include "npt_heap12.h"
#include "npt_overrides.h"
#include "npt_profile.h"
#include "npt_queue.h"
#include "npt_ring.h"
#include "npt_shared.h"
#include "neptune-protocol/npt_protocol_host_dispatch.h"
#include "util/anon_file.h"
#include "util/os_file.h"
#define XXH_INLINE_ALL
#include "util/xxhash.h"
#include "virgl_fence.h"

static uint32_t
hash_uint32(const void *key)
{
   return *(const uint32_t *)key;
}

static bool
equal_uint32(const void *a, const void *b)
{
   return *(const uint32_t *)a == *(const uint32_t *)b;
}

/* Pointer-based IDs share low zero-bits on aligned objects; XXH32
 * spreads them uniformly. */
static uint32_t
hash_uint64(const void *key)
{
   return XXH32(key, sizeof(uint64_t), 0);
}

static bool
equal_uint64(const void *a, const void *b)
{
   return *(const uint64_t *)a == *(const uint64_t *)b;
}

static void
npt_context_ring_monitor_fini(struct npt_context *ctx);

static void
npt_resource_free(struct npt_resource *res)
{
   switch (res->fd_type) {
   case VIRGL_RESOURCE_FD_SHM:
      if (res->u.data && !res->iov_owned)
         munmap(res->u.data, res->size);
      if (res->u.fd >= 0)
         close(res->u.fd);
      break;
   case VIRGL_RESOURCE_FD_DMABUF:
   case VIRGL_RESOURCE_FD_OPAQUE:
      if (res->u.fd >= 0)
         close(res->u.fd);
      break;
   default:
      break;
   }

   free(res);
}

static void
npt_context_free_resource(struct hash_entry *entry)
{
   npt_resource_free(entry->data);
}

void
npt_context_free_detached_resource(struct npt_resource *res)
{
   npt_resource_free(res);
}

/* Overrides are set only where the default dispatcher cannot cope:
 * top-level functions (no _self for dlsym to bind against),
 * shared-HANDLE rejection, and feedback lifecycle hooks.  All other
 * dispatch slots stay NULL and fall through. */
static void
npt_context_init_dispatch(struct npt_context *ctx)
{
   struct npt_dispatch_context *d = &ctx->dispatch;
   memset(d, 0, sizeof(*d));

   d->data = ctx;

   d->decoder = &ctx->decoder;
   d->encoder = &ctx->encoder;

   d->toplevel_dispatch_overrides = &npt_toplevel_overrides;
   d->idxgiresource_dispatch_overrides = &npt_idxgiresource_overrides;
   d->idxgiresource1_dispatch_overrides = &npt_idxgiresource1_overrides;
   d->id3d11device_dispatch_overrides = &npt_id3d11device_overrides;
   d->id3d11device1_dispatch_overrides = &npt_id3d11device1_overrides;
   d->id3d11device5_dispatch_overrides = &npt_id3d11device5_overrides;
   d->id3d11fence_dispatch_overrides = &npt_id3d11fence_overrides;
   d->id3d12device_dispatch_overrides = &npt_id3d12device_overrides;
   d->id3d11devicecontext_dispatch_overrides = &npt_query_dc_overrides;
   d->id3d11devicecontext4_dispatch_overrides = &npt_fence_dc4_overrides;
   d->id3d12commandqueue_dispatch_overrides =
      &npt_id3d12commandqueue_overrides;
   d->id3d12fence_dispatch_overrides = &npt_id3d12fence_overrides;
}

/* `id` is the guest-allocated handle; `host_ptr` is the host-library
 * pointer.  One host pointer can appear under multiple ids (each
 * QueryInterface mints a new id), and each id's COM_RELEASE drops
 * exactly one host-library ref. */
struct npt_object {
   uint64_t id;
   npt_object_type type;
   void *host_ptr;
};

/* Bounded by NPT_OBJECT_TYPE_COUNT so a malformed parent table can't
 * loop. */
static bool
npt_object_type_has_ancestor(npt_object_type t, npt_object_type target)
{
   for (uint32_t i = 0; i < (uint32_t)NPT_OBJECT_TYPE_COUNT; i++) {
      if (t == target)
         return true;
      if (t == 0 || t >= NPT_OBJECT_TYPE_COUNT)
         return false;
      t = npt_object_type_parent[t];
   }
   return false;
}

void
npt_context_register_object(struct npt_context *ctx,
                            uint64_t id,
                            void *obj,
                            npt_object_type type)
{
   if (!id || !obj || !type || !ctx || !ctx->object_table)
      return;

   mtx_lock(&ctx->object_mutex);

   struct hash_entry *entry = _mesa_hash_table_search(ctx->object_table, &id);
   if (entry) {
      struct npt_object *existing = entry->data;
      /* COM objects share one physical pointer across version-
       * compatible interfaces, so QI re-registration with a different
       * type is routine.  Replace IUNKNOWN or upgrade to a derived
       * type; unrelated types log and keep the existing entry (helps
       * catch wrapper-lifetime bugs). */
      if (existing->type != type) {
         if (existing->type == NPT_OBJECT_TYPE_IUNKNOWN ||
             npt_object_type_has_ancestor(type, existing->type)) {
            existing->type = type;
         } else if (type != NPT_OBJECT_TYPE_IUNKNOWN &&
                    !npt_object_type_has_ancestor(existing->type, type)) {
            npt_log("object table: id 0x%016" PRIx64
                    " re-registered with unrelated type %u (was %u)",
                    id, (unsigned)type, (unsigned)existing->type);
         }
      }
      mtx_unlock(&ctx->object_mutex);
      return;
   }

   struct npt_object *e = calloc(1, sizeof(*e));
   if (!e) {
      mtx_unlock(&ctx->object_mutex);
      return;
   }
   e->id = id;
   e->type = type;
   e->host_ptr = obj;

   /* Key points into e->id so the hash key stays valid for e's lifetime. */
   _mesa_hash_table_insert(ctx->object_table, &e->id, e);
   mtx_unlock(&ctx->object_mutex);
}

void
npt_context_unregister_object(struct npt_context *ctx, uint64_t id)
{
   if (!id || !ctx || !ctx->object_table)
      return;

   mtx_lock(&ctx->object_mutex);
   struct hash_entry *entry = _mesa_hash_table_search(ctx->object_table, &id);
   if (entry) {
      free(entry->data);
      _mesa_hash_table_remove(ctx->object_table, entry);
   }
   mtx_unlock(&ctx->object_mutex);
}

/* Accept on exact match, downcast (registered is a descendant of
 * expected), upcast (expected is a descendant of registered), or
 * IUnknown either way.  Upcast is defensive: Create paths can return
 * a base-interface pointer whose host implementation is most-derived
 * in the chain.  D3D/DXGI single-inheritance ABI places the derived
 * vtable at the same physical pointer as the base, so dispatch is
 * still well defined; unrelated chains still fail. */
static bool
npt_object_type_is_compatible(npt_object_type actual,
                              npt_object_type expected)
{
   if (actual == expected)
      return true;
   if (expected == NPT_OBJECT_TYPE_IUNKNOWN ||
       actual == NPT_OBJECT_TYPE_IUNKNOWN)
      return true;
   return npt_object_type_has_ancestor(actual, expected) ||
          npt_object_type_has_ancestor(expected, actual);
}

/* Callers pick an API family with this for an id that is legitimately
 * either one, so a mismatch is an expected answer rather than a violation
 * to diagnose. */
bool
npt_context_object_is(struct npt_context *ctx, uint64_t id,
                      npt_object_type want)
{
   if (!id)
      return false;
   mtx_lock(&ctx->object_mutex);
   const struct hash_entry *entry =
      _mesa_hash_table_search(ctx->object_table, &id);
   const struct npt_object *obj = entry ? entry->data : NULL;
   const npt_object_type actual = obj ? obj->type : (npt_object_type)0;
   mtx_unlock(&ctx->object_mutex);
   return obj && npt_object_type_is_compatible(actual, want);
}

void *
npt_context_lookup_object(struct npt_context *ctx,
                          struct npt_cs_decoder *dec,
                          uint64_t id,
                          npt_object_type expected)
{
   if (!id)
      return NULL;

   const uint64_t prof_t0 =
      npt_profile_enabled() ? npt_profile_now_ns() : 0;
   mtx_lock(&ctx->object_mutex);
   const struct hash_entry *entry =
      _mesa_hash_table_search(ctx->object_table, &id);
   const struct npt_object *obj = entry ? entry->data : NULL;
   const npt_object_type actual = obj ? obj->type : (npt_object_type)0;
   void *host_ptr = obj ? obj->host_ptr : NULL;
   mtx_unlock(&ctx->object_mutex);
   if (npt_profile_enabled()) {
      /* cmd_type=0: per-method attribution happens at the ring loop. */
      npt_profile_record_lookup(npt_profile_now_ns() - prof_t0,
                                !obj || !npt_object_type_is_compatible(
                                          actual, expected),
                                0);
   }

   if (likely(obj && npt_object_type_is_compatible(actual, expected)))
      return host_ptr;

   /* IUNKNOWN lookups are permissive: COM_RELEASE doesn't deref by type
    * and can legitimately race a Create that would register the id. */
   const bool permissive = (expected == NPT_OBJECT_TYPE_IUNKNOWN);
   if (obj) {
      npt_log("object table: id 0x%016" PRIx64 " type mismatch "
              "(expected %u, registered %u)", id,
              (unsigned)expected, (unsigned)actual);
   } else if (!permissive) {
      npt_log("object table: unregistered id 0x%016" PRIx64
              " (expected type %u)", id, (unsigned)expected);
   }

   if (!permissive && dec)
      npt_cs_decoder_set_fatal(dec);
   return NULL;
}

void
npt_context_release_object(struct npt_context *ctx, uint64_t guest_id)
{
   if (!guest_id)
      return;

   /* Permissive: stray RELEASE on an unregistered id (e.g. racing a
    * host-failed QI) is silent; the guest-side wrapper dtor emits
    * COM_RELEASE unconditionally. */
   void *obj = npt_context_lookup_object(ctx, NULL, guest_id,
                                         NPT_OBJECT_TYPE_IUNKNOWN);

   /* Order: feedback_unregister BEFORE IUnknown::Release, otherwise a
    * between-commands poll could call GetData / GetCompletedValue on
    * a freed pointer. */
   npt_feedback_unregister(ctx, guest_id);
   npt_context_unregister_object(ctx, guest_id);

   if (!obj)
      return;

   npt_com_release(obj);

   /* Only after the host library dropped the heap, and with it the
    * import: releasing the pin can complete a deferred munmap. */
   npt_heap12_on_release_object(ctx, guest_id);
}

HRESULT
npt_context_query_interface(struct npt_context *ctx,
                            uint64_t src_guest_id,
                            const GUID *riid,
                            uint64_t new_guest_id)
{
   void *src = npt_context_lookup_object(ctx, NULL, src_guest_id,
                                         NPT_OBJECT_TYPE_IUNKNOWN);
   if (!src || !new_guest_id)
      return NPT_E_NOINTERFACE;

   void *out = NULL;
   HRESULT hr = npt_com_query_interface(src, riid, &out);
   /* Resolve via IID table so later lookups validate exactly; unknown
    * IIDs fall back to IUNKNOWN. */
   if (NPT_SUCCEEDED(hr) && out)
      npt_context_register_object(ctx, new_guest_id, out,
                                  npt_object_type_from_iid(riid));
   return hr;
}

/* Out-of-line because npt_cs.h must not include npt_context.h
 * (header cycle through the generated dispatch types). */
void *
npt_cs_handle_lookup(struct npt_dispatch_context *dispatch,
                     npt_object_id id,
                     npt_object_type type)
{
   if (!id)
      return NULL;
   struct npt_context *ctx = npt_context_from_dispatch(dispatch);
   return npt_context_lookup_object(ctx, dispatch->decoder, id, type);
}

void
npt_cs_handle_register_guest_id(struct npt_dispatch_context *dispatch,
                                uint64_t guest_id,
                                void *obj,
                                npt_object_type type)
{
   if (!guest_id || !obj)
      return;
   struct npt_context *ctx = npt_context_from_dispatch(dispatch);
   npt_context_register_object(ctx, guest_id, obj, type);
}

struct npt_context *
npt_context_create(uint32_t ctx_id,
                   npt_renderer_retire_fence_callback_type retire_fence,
                   size_t debug_len,
                   const char *debug_name)
{
   struct npt_context *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return NULL;

   ctx->ctx_id = ctx_id;
   ctx->retire_fence = retire_fence;

#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   ctx->on_worker_thread = true;
#else
   ctx->on_worker_thread = false;
#endif

   if (debug_name && debug_len) {
      ctx->debug_name = malloc(debug_len + 1);
      if (!ctx->debug_name)
         goto err_debug_name;
      memcpy(ctx->debug_name, debug_name, debug_len);
      ctx->debug_name[debug_len] = '\0';
   }

   if (mtx_init(&ctx->ring_mutex, mtx_plain) != thrd_success)
      goto err_ring_mutex;

   list_inithead(&ctx->rings);

   if (mtx_init(&ctx->wait_ring.mutex, mtx_plain) != thrd_success)
      goto err_wait_ring_mutex;

   if (cnd_init(&ctx->wait_ring.cond) != thrd_success)
      goto err_wait_ring_cond;

   if (mtx_init(&ctx->resource_mutex, mtx_plain) != thrd_success)
      goto err_resource_mutex;

   ctx->resource_table = _mesa_hash_table_create(NULL, hash_uint32, equal_uint32);
   if (!ctx->resource_table)
      goto err_resource_table;

   if (mtx_init(&ctx->pending_blob_mutex, mtx_plain) != thrd_success)
      goto err_pending_blob_mutex;

   ctx->pending_blob_table =
      _mesa_hash_table_create(NULL, hash_uint64, equal_uint64);
   if (!ctx->pending_blob_table)
      goto err_pending_blob_table;

   if (mtx_init(&ctx->heap_import_mutex, mtx_plain) != thrd_success)
      goto err_heap_import_mutex;

   ctx->heap_import_table =
      _mesa_hash_table_create(NULL, hash_uint64, equal_uint64);
   if (!ctx->heap_import_table)
      goto err_heap_import_table;

   if (mtx_init(&ctx->sync_queues_mutex, mtx_plain) != thrd_success)
      goto err_sync_queues_mutex;

   if (!npt_event_init(ctx))
      goto err_event;

   if (mtx_init(&ctx->object_mutex, mtx_plain) != thrd_success)
      goto err_object_mutex;

   ctx->object_table =
      _mesa_hash_table_create(NULL, hash_uint64, equal_uint64);
   if (!ctx->object_table)
      goto err_object_table;

   if (npt_cs_encoder_init(&ctx->encoder, &ctx->cs_fatal_error))
      goto err_encoder;

   if (npt_cs_decoder_init(&ctx->decoder, &ctx->cs_fatal_error))
      goto err_decoder;

   npt_feedback_init(ctx);

   npt_context_init_dispatch(ctx);

   list_inithead(&ctx->head);

   npt_log("created Neptune context %u (%.*s)", ctx_id, (int)debug_len,
           debug_name ? debug_name : "");

   return ctx;

err_decoder:
   npt_cs_encoder_fini(&ctx->encoder);
err_encoder:
   _mesa_hash_table_destroy(ctx->object_table, NULL);
err_object_table:
   mtx_destroy(&ctx->object_mutex);
err_object_mutex:
   npt_event_fini(ctx);
err_event:
   mtx_destroy(&ctx->sync_queues_mutex);
err_sync_queues_mutex:
   _mesa_hash_table_destroy(ctx->heap_import_table, NULL);
err_heap_import_table:
   mtx_destroy(&ctx->heap_import_mutex);
err_heap_import_mutex:
   _mesa_hash_table_destroy(ctx->pending_blob_table, NULL);
err_pending_blob_table:
   mtx_destroy(&ctx->pending_blob_mutex);
err_pending_blob_mutex:
   _mesa_hash_table_destroy(ctx->resource_table, NULL);
err_resource_table:
   mtx_destroy(&ctx->resource_mutex);
err_resource_mutex:
   cnd_destroy(&ctx->wait_ring.cond);
err_wait_ring_cond:
   mtx_destroy(&ctx->wait_ring.mutex);
err_wait_ring_mutex:
   mtx_destroy(&ctx->ring_mutex);
err_ring_mutex:
   free(ctx->debug_name);
err_debug_name:
   free(ctx);
   return NULL;
}

void
npt_context_destroy(struct npt_context *ctx)
{
   npt_log("destroying Neptune context %u", ctx->ctx_id);

   /* Stop the ring monitor before the rings so it can't touch a ring
    * mid-teardown. */
   npt_context_ring_monitor_fini(ctx);

   /* Context destruction is single-threaded; no ring_mutex needed. */
   list_for_each_entry_safe (struct npt_ring, ring, &ctx->rings, head) {
      npt_ring_stop(ring);
      npt_ring_destroy(ring);
   }

   for (uint32_t i = 0; i < ARRAY_SIZE(ctx->sync_queues); i++) {
      if (ctx->sync_queues[i]) {
         npt_queue_destroy(ctx->sync_queues[i]);
         ctx->sync_queues[i] = NULL;
      }
   }
   mtx_destroy(&ctx->sync_queues_mutex);

   /* Retire fences still parked awaiting their ARM before the event
    * state (and its lists) go away, so guest waiters unblock. */
   npt_event_drain_parked_fences(ctx);
   npt_event_fini(ctx);

   /* Guests are expected to release every COM object before context
    * teardown.  Free any straggler entries to plug a guest leak. */
   hash_table_foreach(ctx->object_table, entry) {
      free(entry->data);
   }
   _mesa_hash_table_destroy(ctx->object_table, NULL);
   mtx_destroy(&ctx->object_mutex);

   npt_feedback_fini(ctx);

   npt_cs_decoder_fini(&ctx->decoder);
   npt_cs_encoder_fini(&ctx->encoder);

   /* Before the resource table goes away, so a zombie held up only by
    * the import table is not freed twice. */
   npt_heap12_context_fini(ctx);
   _mesa_hash_table_destroy(ctx->heap_import_table, NULL);
   mtx_destroy(&ctx->heap_import_mutex);

   _mesa_hash_table_destroy(ctx->resource_table, npt_context_free_resource);
   mtx_destroy(&ctx->resource_mutex);

   if (ctx->sync_maps.count)
      npt_log("context destroyed with %u sync map(s) still active",
              ctx->sync_maps.count);
   free(ctx->sync_maps.entries);

   hash_table_foreach(ctx->pending_blob_table, entry) {
      struct npt_pending_blob *pb = entry->data;
      if (pb->fd >= 0)
         close(pb->fd);
      free(pb);
   }
   _mesa_hash_table_destroy(ctx->pending_blob_table, NULL);
   mtx_destroy(&ctx->pending_blob_mutex);

   cnd_destroy(&ctx->wait_ring.cond);
   mtx_destroy(&ctx->wait_ring.mutex);

   mtx_destroy(&ctx->ring_mutex);

   free(ctx->debug_name);
   free(ctx);
}

void
npt_context_on_ring_seqno_update(struct npt_context *ctx,
                                 uint64_t ring_id,
                                 uint32_t ring_seqno)
{
   /* Steady-state fast path skips the per-command mutex.  Writers
    * publish .id under the mutex with release semantics; an acquire
    * load returning 0 or a different ring_id means there is nothing
    * to signal.  The wait path re-checks the ring head after setting
    * .id, so a wakeup racing with a setter falls through to the
    * producer's next call. */
   if (likely(atomic_load_explicit(&ctx->wait_ring.id,
                                   memory_order_acquire) != ring_id))
      return;

   mtx_lock(&ctx->wait_ring.mutex);
   if (ctx->wait_ring.id == ring_id &&
       npt_seqno_ge(ring_seqno, ctx->wait_ring.seqno))
      cnd_signal(&ctx->wait_ring.cond);
   mtx_unlock(&ctx->wait_ring.mutex);
}

bool
npt_context_get_wait_ring_seqno(struct npt_context *ctx,
                                uint64_t ring_id,
                                uint32_t *out_seqno)
{
   bool wait_ring = false;
   mtx_lock(&ctx->wait_ring.mutex);
   if (ctx->wait_ring.id == ring_id) {
      wait_ring = true;
      *out_seqno = ctx->wait_ring.seqno;
   }
   mtx_unlock(&ctx->wait_ring.mutex);
   return wait_ring;
}

static void
npt_context_wake_rings(struct npt_context *ctx,
                       void (*wake)(struct npt_ring *ring))
{
   mtx_lock(&ctx->ring_mutex);
   list_for_each_entry(struct npt_ring, r, &ctx->rings, head) {
      wake(r);
   }
   mtx_unlock(&ctx->ring_mutex);
}

/* A feedback entry can be armed from a thread other than the ring
 * thread that will poll it, and a ring thread with an empty pending list
 * parks in an unbounded wait.  Without this the entry would sit unpolled
 * and the guest would wait forever on a value that is never published. */
void
npt_context_notify_rings_feedback(struct npt_context *ctx)
{
   if (!ctx)
      return;

   npt_context_wake_rings(ctx, npt_ring_notify_feedback);
}

void
npt_context_on_ring_fatal(struct npt_context *ctx)
{
   ctx->cs_fatal_error = true;

   mtx_lock(&ctx->wait_ring.mutex);
   cnd_signal(&ctx->wait_ring.cond);
   mtx_unlock(&ctx->wait_ring.mutex);

   /* Wake any ring thread parked in WAIT_VIRTQUEUE_SEQNO or idle so
    * it observes the fatal flag (both waits share ring->mutex/cond). */
   npt_context_wake_rings(ctx, npt_ring_wake);
}

/* Ring monitor (ALIVE-bit reporter) */

static struct timespec
npt_context_timespec_add(struct timespec a, struct timespec b)
{
   const long NS_PER_SEC = 1000000000;
   a.tv_sec += b.tv_sec;
   a.tv_nsec += b.tv_nsec;
   if (a.tv_nsec >= NS_PER_SEC) {
      a.tv_sec += 1;
      a.tv_nsec -= NS_PER_SEC;
   }
   return a;
}

static int
npt_context_ring_monitor_thread(void *arg)
{
   struct npt_context *ctx = arg;

   char thread_name[16];
   snprintf(thread_name, ARRAY_SIZE(thread_name), "npt-mon-%u", ctx->ctx_id);
   u_thread_setname(thread_name);

   int ret = thrd_busy;
   while (ctx->ring_monitor.started) {
      /* Fire ALIVE on first iteration AND on every period timeout.
       * Treating thrd_timeout as an error would silently wedge the
       * guest watchdog on the first period; only real errors break. */
      if (ret == thrd_busy || ret == thrd_timeout) {
         mtx_lock(&ctx->ring_mutex);
         list_for_each_entry (struct npt_ring, ring, &ctx->rings, head) {
            if (ring->monitor)
               npt_ring_set_status_bits(ring, NPT_RING_STATUS_ALIVE_BIT);
         }
         mtx_unlock(&ctx->ring_mutex);
         ret = 0;
      } else if (ret) {
         break;
      }
      /* ret == 0: thrd_success (explicit signal from monitor_fini or
       * period-tightening); re-check started flag at top of loop. */

      mtx_lock(&ctx->ring_monitor.mutex);
      struct timespec ts;
      if ((ret = clock_gettime(CLOCK_REALTIME, &ts))) {
         mtx_unlock(&ctx->ring_monitor.mutex);
         break;
      }

      const uint32_t period_us = ctx->ring_monitor.report_period_us;
      ts = npt_context_timespec_add(
         ts, (struct timespec){ period_us / 1000000,
                                (period_us % 1000000) * 1000 });
      ret = cnd_timedwait(&ctx->ring_monitor.cond, &ctx->ring_monitor.mutex,
                          &ts);
      mtx_unlock(&ctx->ring_monitor.mutex);
   }

   return ret;
}

bool
npt_context_ring_monitor_init(struct npt_context *ctx,
                              uint32_t report_period_us)
{
   assert(report_period_us > 0);

   /* Already started: tighten the period if shorter, kick the thread
    * so it observes the new period. */
   if (ctx->ring_monitor.started) {
      mtx_lock(&ctx->ring_monitor.mutex);
      if (report_period_us < ctx->ring_monitor.report_period_us) {
         ctx->ring_monitor.report_period_us = report_period_us;
         cnd_signal(&ctx->ring_monitor.cond);
      }
      mtx_unlock(&ctx->ring_monitor.mutex);
      return true;
   }

   if (mtx_init(&ctx->ring_monitor.mutex, mtx_plain) != thrd_success)
      goto err_mtx;
   if (cnd_init(&ctx->ring_monitor.cond) != thrd_success)
      goto err_cnd;

   ctx->ring_monitor.report_period_us = report_period_us;
   ctx->ring_monitor.started = true;

   if (thrd_create(&ctx->ring_monitor.thread, npt_context_ring_monitor_thread,
                   ctx) != thrd_success)
      goto err_thrd;

   return true;

err_thrd:
   ctx->ring_monitor.started = false;
   cnd_destroy(&ctx->ring_monitor.cond);
err_cnd:
   mtx_destroy(&ctx->ring_monitor.mutex);
err_mtx:
   return false;
}

static void
npt_context_ring_monitor_fini(struct npt_context *ctx)
{
   if (!ctx->ring_monitor.started)
      return;

   mtx_lock(&ctx->ring_monitor.mutex);
   ctx->ring_monitor.started = false;
   cnd_signal(&ctx->ring_monitor.cond);
   mtx_unlock(&ctx->ring_monitor.mutex);

   thrd_join(ctx->ring_monitor.thread, NULL);

   cnd_destroy(&ctx->ring_monitor.cond);
   mtx_destroy(&ctx->ring_monitor.mutex);
}

bool
npt_context_wait_ring_seqno(struct npt_context *ctx,
                            struct npt_ring *ring,
                            uint32_t ring_seqno)
{
   bool ok = true;

   mtx_lock(&ctx->wait_ring.mutex);
   /* Release-store so the producer's lockless acquire-load in
    * on_ring_seqno_update sees this assignment before any subsequent
    * cmd-publish + on_ring_seqno_update races to wake us. */
   atomic_store_explicit(&ctx->wait_ring.id, ring->id,
                         memory_order_release);
   ctx->wait_ring.seqno = ring_seqno;
   while (!ctx->cs_fatal_error && ok &&
          !npt_seqno_ge(npt_ring_load_head(ring), ring_seqno)) {
      ok = cnd_wait(&ctx->wait_ring.cond, &ctx->wait_ring.mutex) ==
           thrd_success;
   }
   atomic_store_explicit(&ctx->wait_ring.id, (uint64_t)0,
                         memory_order_release);
   mtx_unlock(&ctx->wait_ring.mutex);

   return ok;
}

/* cmd_type groups: 0 = transport (consumer-defined), 1-3 = top-level,
 * 255 = COM.  Transport commands must be intercepted here; the
 * protocol's default dispatcher sets fatal on them. */
bool
npt_context_dispatch_one_command(struct npt_context *ctx,
                                 struct npt_dispatch_context *dispatch,
                                 struct npt_cs_decoder *dec,
                                 struct npt_cs_encoder *enc)
{
   struct npt_command_header peek;
   npt_cs_decoder_peek(dec, sizeof(peek), &peek, sizeof(peek));
   if (npt_cs_decoder_get_fatal(dec))
      return false;

   const uint32_t group = (peek.cmd_type >> 24) & 0xFFu;

   if (group == 0) {
      npt_cs_decoder_read(dec, sizeof(peek), &peek, sizeof(peek));
      if (!npt_transport_dispatch(ctx, dispatch, dec, enc, &peek)) {
         npt_log("unhandled transport command: cmd_type=0x%08x",
                 peek.cmd_type);
         npt_cs_decoder_set_fatal(dec);
         return false;
      }
      /* A matched transport handler may still set fatal; the common
       * tail check catches that. */
   } else {
      /* Generated dispatcher reads its own header. */
      npt_dispatch_command(dispatch);
      /* Event handles the decode substituted stay pinned for the
       * duration of the call they were passed to. */
      npt_event_unpin_dispatched(ctx);
   }

   return !npt_cs_decoder_get_fatal(dec);
}

bool
npt_context_submit_cmd(struct npt_context *ctx, const void *buffer, size_t size)
{
   if (ctx->cs_fatal_error)
      return false;

   struct npt_cs_decoder *dec = &ctx->decoder;
   npt_cs_decoder_set_buffer_stream(dec, buffer, size);

   while (npt_cs_decoder_has_command(dec)) {
      if (!npt_context_dispatch_one_command(ctx, &ctx->dispatch, dec,
                                            &ctx->encoder)) {
         npt_log("context %u: dispatch failed", ctx->ctx_id);
         npt_cs_decoder_reset(dec);
         return false;
      }
   }

   npt_cs_decoder_reset(dec);
   return true;
}

bool
npt_context_submit_fence(struct npt_context *ctx,
                         uint32_t ring_idx,
                         uint64_t fence_id)
{
   /* ring_idx 0 = CPU timeline, retire immediately. */
   if (ring_idx == 0) {
      ctx->retire_fence(ctx->ctx_id, ring_idx, fence_id);
      return true;
   }

   if (ring_idx >= ARRAY_SIZE(ctx->sync_queues)) {
      npt_log("submit_fence: invalid ring_idx %u", ring_idx);
      return false;
   }

   /* Non-event rings (below NPT_EVENT_RING_BASE) carry no wait source;
    * retire on the CPU timeline. */
   if (ring_idx < NPT_EVENT_RING_BASE) {
      ctx->retire_fence(ctx->ctx_id, ring_idx, fence_id);
      return true;
   }

   /* Event rings: the wait source comes from a pending ARM_EVENT_FENCE.
    * Fence and ARM travel on independent channels, so either may arrive
    * first; pop the ARM or park the fence atomically.  Retiring on a
    * miss is not an option -- it completes the guest's fence wait before
    * the GPU work has run. */
   struct npt_event_paired paired;
   int sync_fd = npt_event_pop_arm_or_park_fence(ctx, ring_idx, fence_id,
                                                 &paired);
   if (sync_fd == NPT_EVENT_FENCE_PARKED)
      return true;
   if (sync_fd < 0) {
      /* Allocation failure: retiring early is still wrong, but wedging
       * the guest is worse.  Loud. */
      npt_log("submit_fence: PARK FAILED (ring=%u id=%" PRIu64 ") -- "
              "retiring EARLY", ring_idx, fence_id);
      ctx->retire_fence(ctx->ctx_id, ring_idx, fence_id);
      return true;
   }

   return npt_context_pair_event_fence(ctx, ring_idx, fence_id,
                                       &paired, /*register_fd=*/true);
}

bool
npt_context_pair_event_fence(struct npt_context *ctx,
                             uint32_t ring_idx, uint64_t fence_id,
                             const struct npt_event_paired *paired,
                             bool register_fd)
{
   int sync_fd = paired->fd;
   /* No vkGetDeviceQueue2 equivalent: create the sync queue on
    * first use for a given ring_idx. */
   mtx_lock(&ctx->sync_queues_mutex);
   struct npt_queue *queue = ctx->sync_queues[ring_idx];
   if (!queue) {
      queue = npt_queue_create(ctx, ring_idx);
      if (!queue) {
         mtx_unlock(&ctx->sync_queues_mutex);
         npt_log("pair_event_fence: failed to create sync queue for "
                 "ring_idx %u", ring_idx);
         close(sync_fd);
         npt_event_release_proxy(ctx, paired->release_proxy);
         if (paired->check_fence)
            npt_d3d12_gate_release(paired->check_fence);
         ctx->retire_fence(ctx->ctx_id, ring_idx, fence_id);
         return true;
      }
      ctx->sync_queues[ring_idx] = queue;
   }
   mtx_unlock(&ctx->sync_queues_mutex);

   /* Register so render_context_dispatch_submit_fence can retrieve it
    * via virgl_renderer_get_fence_fd and pass it via SCM_RIGHTS to
    * the proxy.  virgl_fence_set_fd dups internally, so we still own
    * sync_fd here.
    *
    * Key by the fence's (ring_idx, fence_id) identity: the guest's
    * seqno is per-ring and repeats across the event rings, so the
    * render-server table keys by the full pair, matching the lookup in
    * render_context_dispatch_submit_fence.
    *
    * Only on the synchronous submit path: virgl_fence_take_fd runs
    * right after we return, making the entry a one-shot hand-off.  A
    * parked fence's consumer replied long ago with has_fd=0, so an entry
    * registered for it strands -- and virgl_fence_set_fd sweeps every
    * live entry with a syscall on each call, so strands make every later
    * arm more expensive for the rest of the run.  Retirement rides the
    * retire_fence message, not this fd. */
   if (register_fd) {
      int err = virgl_fence_set_fd(virgl_fence_ring_key(ring_idx, fence_id),
                                   sync_fd);
      if (err)
         npt_log("pair_event_fence: virgl_fence_set_fd(ring=%u, id=%" PRIu64
                 ") failed (err=%d)", ring_idx, fence_id, err);
   }

   if (!npt_queue_sync_submit(queue, ring_idx, fence_id, paired)) {
      /* The submit released the paired wait source, so nothing is left
       * that could ever retire this fence.  Retiring early is wrong;
       * leaving the guest's wait outstanding for good is worse. */
      npt_log("pair_event_fence: sync submit failed (ring=%u id=%" PRIu64
              ") -- retiring EARLY", ring_idx, fence_id);
      ctx->retire_fence(ctx->ctx_id, ring_idx, fence_id);
      return false;
   }

   return true;
}

bool
npt_context_create_resource(struct npt_context *ctx,
                            uint32_t res_id,
                            uint64_t blob_id,
                            uint64_t blob_size,
                            UNUSED uint32_t blob_flags,
                            struct virgl_context_blob *out_blob)
{
   if (blob_id == 0) {
      /* SHM resource (ring buffer or generic shared memory). */
      int fd = os_create_anonymous_file(blob_size, "npt-shmem");
      if (fd < 0)
         return false;

      void *data = mmap(NULL, blob_size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
      if (data == MAP_FAILED) {
         close(fd);
         return false;
      }

      struct npt_resource *res = calloc(1, sizeof(*res));
      if (!res) {
         munmap(data, blob_size);
         close(fd);
         return false;
      }

      res->res_id = res_id;
      res->fd_type = VIRGL_RESOURCE_FD_SHM;
      res->size = blob_size;
      res->u.data = data;

#ifdef __linux__
      /* Seal against shrink so the memfd can back a udmabuf for a D3D12
       * heap import.  Failure is not fatal: UDMABUF_CREATE then fails
       * cleanly and the guest degrades to the sync-map path. */
      if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0)
         npt_log("create_resource: F_ADD_SEALS(F_SEAL_SHRINK) on res %u "
                 "failed (errno %d); heap imports will fall back",
                 res_id, errno);
#endif

      /* Ownership of the original fd leaves via out_blob, so keep a dup
       * for a later heap import to wrap. */
      res->u.fd = dup(fd);
      if (res->u.fd < 0)
         npt_log("create_resource: dup(fd) for res %u failed (errno %d); "
                 "heap imports will fall back", res_id, errno);

      mtx_lock(&ctx->resource_mutex);
      if (_mesa_hash_table_search(ctx->resource_table, &res->res_id)) {
         mtx_unlock(&ctx->resource_mutex);
         npt_log("create_resource: duplicate res_id %u", res_id);
         munmap(data, blob_size);
         if (res->u.fd >= 0)
            close(res->u.fd);
         close(fd);
         free(res);
         return false;
      }
      _mesa_hash_table_insert(ctx->resource_table, &res->res_id, res);
      mtx_unlock(&ctx->resource_mutex);

      /* CACHED, not WC: the host maps these pages write-back, so a WC
       * guest mapping of the same physical pages would alias two
       * incoherent cacheability attributes.  Persistently-mapped heaps
       * also hand these pages straight to the app, where write-back is
       * mandatory for read performance. */
      *out_blob = (struct virgl_context_blob){
         .type = VIRGL_RESOURCE_FD_SHM,
         .u.fd = fd,
         .map_info = VIRGL_RENDERER_MAP_CACHE_CACHED,
      };

      return true;
   } else {
      /* Server-created blob (a shared texture's dmabuf export) staged
       * earlier by EXPORT_BLOB. */
      mtx_lock(&ctx->pending_blob_mutex);
      struct hash_entry *entry =
         _mesa_hash_table_search(ctx->pending_blob_table, &blob_id);
      struct npt_pending_blob *pb = entry ? entry->data : NULL;
      if (pb)
         _mesa_hash_table_remove(ctx->pending_blob_table, entry);
      mtx_unlock(&ctx->pending_blob_mutex);

      if (!pb)
         return false;

      /* Record the binding in the resource table too, so a same-
       * context SHARED_OPEN_RES on this res_id resolves without an
       * attach round-trip (attach is deduped for the creating
       * context).  The table owns its own dup. */
      if (pb->fd_type == NPT_SHARED_FD_TYPE) {
         int table_fd = dup(pb->fd);
         if (table_fd >= 0 &&
             !npt_context_import_resource(ctx, res_id, pb->fd_type,
                                          table_fd, pb->size))
            close(table_fd);
      }

      *out_blob = (struct virgl_context_blob){
         .type = pb->fd_type,
         .u.fd = pb->fd,
         .map_info = 0,
         .export_format = pb->virgl_format,
      };
      free(pb);

      return true;
   }
}

bool
npt_context_register_pending_blob(struct npt_context *ctx,
                                  uint64_t blob_id,
                                  enum virgl_resource_fd_type fd_type,
                                  int fd,
                                  uint64_t size,
                                  uint32_t virgl_format)
{
   struct npt_pending_blob *pb = calloc(1, sizeof(*pb));
   if (!pb)
      return false;

   pb->blob_id = blob_id;
   pb->fd_type = fd_type;
   pb->fd = fd;
   pb->size = size;
   pb->virgl_format = virgl_format;

   mtx_lock(&ctx->pending_blob_mutex);
   _mesa_hash_table_insert(ctx->pending_blob_table, &pb->blob_id, pb);
   mtx_unlock(&ctx->pending_blob_mutex);

   return true;
}

bool
npt_context_import_resource(struct npt_context *ctx,
                            uint32_t res_id,
                            enum virgl_resource_fd_type fd_type,
                            int fd,
                            uint64_t size)
{
   struct npt_resource *res = calloc(1, sizeof(*res));
   if (!res)
      return false;

   res->res_id = res_id;
   res->fd_type = fd_type;
   res->size = size;

   /* Defer fd ownership until the hash-table insert succeeds: on
    * duplicate-id rejection the caller still owns the original fd. */
   switch (fd_type) {
   case VIRGL_RESOURCE_FD_SHM:
      res->u.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (res->u.data == MAP_FAILED) {
         free(res);
         return false;
      }
      break;
   case VIRGL_RESOURCE_FD_DMABUF:
   case VIRGL_RESOURCE_FD_OPAQUE:
      break;
   default:
      free(res);
      return false;
   }

   mtx_lock(&ctx->resource_mutex);
   if (_mesa_hash_table_search(ctx->resource_table, &res->res_id)) {
      mtx_unlock(&ctx->resource_mutex);
      npt_log("import_resource: duplicate res_id %u", res_id);
      if (fd_type == VIRGL_RESOURCE_FD_SHM && res->u.data && res->u.data != MAP_FAILED)
         munmap(res->u.data, size);
      free(res);
      return false;
   }

   /* Take fd ownership.  For shm the mapping serves the data windows,
    * but the fd is kept as well: on hosts where shared textures ride
    * shm, SHARED_OPEN_RES re-exports it to the D3D library. */
   res->u.fd = fd;
   _mesa_hash_table_insert(ctx->resource_table, &res->res_id, res);
   mtx_unlock(&ctx->resource_mutex);

   return true;
}

void
npt_context_destroy_resource(struct npt_context *ctx, uint32_t res_id)
{
   /* Detach the hash entry first; the local `res` pointer survives as
    * the stream-identity cookie for the ring scan below. */
   mtx_lock(&ctx->resource_mutex);
   struct hash_entry *entry =
      _mesa_hash_table_search(ctx->resource_table, &res_id);
   struct npt_resource *res = entry ? entry->data : NULL;
   if (entry)
      _mesa_hash_table_remove(ctx->resource_table, entry);
   mtx_unlock(&ctx->resource_mutex);

   if (!res)
      return;

   /* Mark fatal if the resource is the active stream of the encoder
    * or any ring. */
   if (!npt_cs_encoder_check_stream(&ctx->encoder, res))
      ctx->cs_fatal_error = true;

   /* Detach doomed rings under ring_mutex, then stop+destroy after
    * dropping it: npt_ring_stop joins the ring thread, which can
    * re-enter via on_ring_seqno_update — holding ring_mutex across
    * the join would deadlock. */
   struct list_head doomed;
   list_inithead(&doomed);

   mtx_lock(&ctx->ring_mutex);
   list_for_each_entry_safe (struct npt_ring, ring, &ctx->rings, head) {
      if (ring->resource == res ||
          !npt_cs_decoder_check_stream(&ring->decoder, res) ||
          !npt_cs_encoder_check_stream(&ring->encoder, res)) {
         ctx->cs_fatal_error = true;
         list_del(&ring->head);
         list_addtail(&ring->head, &doomed);
      }
   }
   mtx_unlock(&ctx->ring_mutex);

   list_for_each_entry_safe (struct npt_ring, ring, &doomed, head) {
      npt_ring_stop(ring);
      npt_ring_destroy(ring);
   }

   /* A live ID3D12Heap import still aliases this mapping, so munmapping
    * it now is undefined under the host graphics driver.  Park the
    * detached entry as a zombie for the last import to free. */
   mtx_lock(&ctx->resource_mutex);
   const uint32_t imports = res->heap_import_count;
   if (imports > 0)
      res->zombie = true;
   mtx_unlock(&ctx->resource_mutex);

   if (imports > 0) {
      npt_log("destroy_resource: res %u still imported by %u D3D12 heap(s); "
              "DEFERRING munmap until heap release (zombie)",
              res_id, imports);
      return;
   }

   npt_resource_free(res);
}
