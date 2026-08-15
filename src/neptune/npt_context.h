/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-renderer-context Neptune state.
 */

#ifndef NPT_CONTEXT_H
#define NPT_CONTEXT_H

#include "npt_common.h"
#include "npt_cs.h"
#include "npt_event.h"
#include "npt_feedback.h"
#include "npt_renderer.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "virgl_resource.h"

struct npt_queue;

/* Server-created resource (a shared texture's dmabuf export) that the
 * guest later claims via create_blob. */
struct npt_pending_blob {
   uint64_t blob_id;
   enum virgl_resource_fd_type fd_type;
   int fd;
   uint64_t size;
   /* enum virgl_formats the exporting context created the texture with,
    * or 0 if unknown.  See virgl_resource::export_format. */
   uint32_t virgl_format;
};

/* Sync Map/Unmap bookkeeping.  Keyed by (resource_id, subresource):
 * the map pool packs many resources' slots into ONE shm blob, so state
 * cannot live on the blob — two overlapping sync maps of pool
 * neighbours would clobber each other's mapped_data and mismatch their
 * unmaps.  Kept in a per-context table (all map/unmap for a context
 * run on its ring thread; no locking needed). */
struct npt_sync_map_entry {
   uint64_t resource_id;
   uint32_t subresource;
   uint32_t access_flags;  /* NPT_MAP_ACCESS_* — decides copy direction */
   void    *mapped_data;   /* host pointer returned by D3D11/12 Map */
   uint32_t mapped_size;
   bool     persistent;    /* D3D12 persistent map: no Unmap expected */
};

struct npt_resource {
   uint32_t res_id;

   enum virgl_resource_fd_type fd_type;
   bool iov_owned; /* data borrowed from IOV: don't munmap */

   /* A struct, not a union: an imported shm resource keeps BOTH its
    * mapping (ring/feedback/transfer windows read data) and its fd
    * (on hosts where shared textures ride shm, SHARED_OPEN_RES hands
    * the fd back to the D3D library).  fd is -1 when absent (e.g.
    * iov-backed guest storage). */
   struct {
      int fd;          /* dma_buf / opaque / shm export */
      uint8_t *data;   /* shm mapping */
   } u;

   size_t size;

   /* Live ID3D12Heap imports aliasing u.data, plus any import in
    * progress.  While nonzero the mapping must NOT be munmapped: destroy
    * marks the entry `zombie` instead and the last import released
    * completes the free.  Both fields are guarded by resource_mutex. */
   uint32_t heap_import_count;
   bool zombie;
};

struct npt_context {
   uint32_t ctx_id;

   /* Caller-supplied tag for log lines (typically the guest process
    * name).  May be NULL. */
   char *debug_name;

   /* True when this context runs on a virgl_render_server worker
    * thread; false in the default worker-process mode. */
   bool on_worker_thread;

   npt_renderer_retire_fence_callback_type retire_fence;

   mtx_t ring_mutex;
   struct list_head rings;

   /* Watchdog-reporter thread, lazily started by CREATE_RING.  OR-sets
    * NPT_RING_STATUS_ALIVE_BIT on every monitored ring at a period
    * equal to the smallest monitor_report_period_us requested across
    * all rings. */
   struct {
      mtx_t mutex;
      cnd_t cond;
      thrd_t thread;
      atomic_bool started;
      atomic_uint report_period_us;
   } ring_monitor;

   /* Single-waiter park lot: block until ring->id reaches a target
    * seqno.  Ring thread signals on progress and on fatal teardown.
    *
    * .id is atomic so the per-command on_ring_seqno_update can fast-
    * path via lockless load when no waiter is installed (the steady-
    * state case).  Writes to .id always happen under the mutex; the
    * atomic just lets the producer poll without contending for it. */
   struct {
      mtx_t mutex;
      cnd_t cond;
      _Atomic uint64_t id;
      uint32_t seqno;
   } wait_ring;

   mtx_t resource_mutex;
   struct hash_table *resource_table;

   /* Active sync maps, keyed (resource_id, subresource); see
    * npt_sync_map_entry.  Touched only from the ring thread. */
   struct {
      struct npt_sync_map_entry *entries;
      uint32_t count;
      uint32_t cap;
   } sync_maps;

   /* Object handle table.  Maps guest-visible object_id (host COM
    * pointer cast to uint64_t) to npt_object_type, so handle lookup
    * can reject ids the guest never obtained from Create* / QI.
    * Drained by COM_RELEASE and context_destroy. */
   mtx_t object_mutex;
   struct hash_table *object_table;

   mtx_t pending_blob_mutex;
   struct hash_table *pending_blob_table;  /* blob_id -> npt_pending_blob */

   /* Shmem-imported D3D12 heaps: heap guest id -> backing SHM
    * npt_resource, consulted on COM_RELEASE to drop that resource's
    * import pin. */
   mtx_t heap_import_mutex;
   struct hash_table *heap_import_table;

   /* Indexed by guest-supplied ring_idx; entry 0 is unused (ring_idx
    * 0 means "retire on CPU timeline").  Lazily populated when the
    * first fence on a given ring_idx arrives.
    *
    * Partition:
    *   0                          : CPU timeline
    *   [1, NPT_EVENT_RING_BASE)   : reserved
    *   [NPT_EVENT_RING_BASE, end) : Win32 event proxies
    *
    * NPT_EVENT_RING_BASE must equal (sync_queues count / 2) in
    * lockstep with the guest driver. */
   mtx_t sync_queues_mutex;
   struct npt_queue *sync_queues[64];

   /* Per-ring gate events for D3D12 monitored-fence gates, kept alive
    * until context teardown so the host D3D library's asynchronous
    * completion fires always land on a live object of the right ring.
    * Guarded by event_mutex. */
   struct npt_event_fd gate_ring[64];

   /* Win32 event HANDLE emulation: each proxy owns an eventfd handed
    * to the host D3D library as the HANDLE.  event_pending_arms
    * carries (ring_idx, dup_fd) triples; the next matching
    * submit_fence pops one and feeds dup_fd to the sync-queue worker. */
   mtx_t                 event_mutex;
   struct hash_table    *event_proxies;
   struct list_head      event_pending_arms;
   /* Fences that arrived before their ARM_EVENT_FENCE was decoded: the
    * Windows KMD delivers the fence on the virtio control queue while
    * the ARM sits in the npt event ring, so either can win.  Parked here
    * under event_mutex and paired when the ARM lands -- retiring on the
    * miss would complete the guest's wait before the GPU work ran, and
    * leave the arm to mispair with the ring's next fence. */
   struct list_head      event_pending_fences;

   /* Per-object shmem slots (queries, fences, ...) the dispatch
    * thread writes when host state advances, so the guest reads
    * results locally instead of round-tripping.  See npt_feedback. */
   struct npt_feedback_state feedback;

   bool cs_fatal_error;
   struct npt_cs_encoder encoder;
   struct npt_cs_decoder decoder;

   /* dispatch.data = ctx; override functions recover the outer
    * struct via npt_context_from_dispatch. */
   struct npt_dispatch_context dispatch;

   struct list_head head;
};

/* Half of sync_queues[] is reserved rings, half is event proxies.
 * Must be (sizeof sync_queues / sizeof sync_queues[0]) / 2 in lockstep
 * with the guest driver. */
#define NPT_EVENT_RING_BASE 32u

struct npt_context *
npt_context_create(uint32_t ctx_id,
                   npt_renderer_retire_fence_callback_type retire_fence,
                   size_t debug_len,
                   const char *debug_name);

void
npt_context_destroy(struct npt_context *ctx);

bool
npt_context_submit_cmd(struct npt_context *ctx, const void *buffer, size_t size);

/* Peek one command header, route to transport dispatch (group 0) or
 * the generated protocol dispatcher otherwise.  Shared by context and
 * ring submit_cmd loops.  Returns false on group-0 with no handler
 * (decoder left fatal); otherwise true (caller checks get_fatal). */
bool
npt_context_dispatch_one_command(struct npt_context *ctx,
                                 struct npt_dispatch_context *dispatch,
                                 struct npt_cs_decoder *dec,
                                 struct npt_cs_encoder *enc);

/* ring_idx 0 retires immediately on the CPU timeline; event rings
 * (>= NPT_EVENT_RING_BASE) wait on the pending ARM_EVENT_FENCE's
 * proxy eventfd via a per-ring npt_queue worker. */
bool
npt_context_submit_fence(struct npt_context *ctx,
                         uint32_t ring_idx,
                         uint64_t fence_id);

/* Route a paired (arm wait fd, fence) to the ring's sync queue, taking
 * ownership of the fd.
 *
 * register_fd puts the fd in the virgl fence table for the submit
 * dispatch's virgl_fence_take_fd, and is true ONLY on the synchronous
 * submit_fence path.  The parked-pairing paths pass false: their
 * consumer replied long ago, and a stranded entry is swept with a
 * syscall by every later virgl_fence_set_fd, forever. */
struct npt_event_paired;
bool
npt_context_pair_event_fence(struct npt_context *ctx,
                             uint32_t ring_idx, uint64_t fence_id,
                             const struct npt_event_paired *paired,
                             bool register_fd);

bool
npt_context_create_resource(struct npt_context *ctx,
                            uint32_t res_id,
                            uint64_t blob_id,
                            uint64_t blob_size,
                            uint32_t blob_flags,
                            struct virgl_context_blob *out_blob);

bool
npt_context_import_resource(struct npt_context *ctx,
                            uint32_t res_id,
                            enum virgl_resource_fd_type fd_type,
                            int fd,
                            uint64_t size);

void
npt_context_destroy_resource(struct npt_context *ctx, uint32_t res_id);

/* Free a resource already detached from resource_table, completing a
 * munmap that was deferred while it was pinned. */
void
npt_context_free_detached_resource(struct npt_resource *res);

static inline struct npt_resource *
npt_context_get_resource(struct npt_context *ctx, uint32_t res_id)
{
   mtx_lock(&ctx->resource_mutex);
   const struct hash_entry *entry =
      _mesa_hash_table_search(ctx->resource_table, &res_id);
   mtx_unlock(&ctx->resource_mutex);

   return likely(entry) ? entry->data : NULL;
}

/* Takes ownership of the fd. */
bool
npt_context_register_pending_blob(struct npt_context *ctx,
                                  uint64_t blob_id,
                                  enum virgl_resource_fd_type fd_type,
                                  int fd,
                                  uint64_t size,
                                  uint32_t virgl_format);

/* Wake any wait_ring waiter on \p ring_id whose target seqno is
 * reached.  Called after each dispatched command. */
void
npt_context_on_ring_seqno_update(struct npt_context *ctx,
                                 uint64_t ring_id,
                                 uint32_t ring_seqno);

/* Tell every ring thread of this context that a feedback poll is owed,
 * so an entry armed off the ring thread can never be left unpolled.
 * Must NOT be called holding the feedback state lock (it takes
 * ring->mutex, which the poll path nests the other way round). */
void
npt_context_notify_rings_feedback(struct npt_context *ctx);

/* Mark the context fatal and wake any wait_ring waiter. */
void
npt_context_on_ring_fatal(struct npt_context *ctx);

/* Idempotent.  Tightens the reporting rate when called with a shorter
 * period than the current one.  Returns false if the thread can't start. */
bool
npt_context_ring_monitor_init(struct npt_context *ctx,
                              uint32_t report_period_us);

/* Block until ring->id advances past ring_seqno or the context goes
 * fatal.  Returns false on cnd_wait failure; true on normal progress
 * AND on fatal-observed exit. */
struct npt_ring;
bool
npt_context_wait_ring_seqno(struct npt_context *ctx,
                            struct npt_ring *ring,
                            uint32_t ring_seqno);

/* Idle-path probe.  True (and writes *out_seqno) when the context is
 * currently waiting on ring_id; used by the ring thread to detect a
 * driver-supplied seqno the ring can never reach. */
bool
npt_context_get_wait_ring_seqno(struct npt_context *ctx,
                                uint64_t ring_id,
                                uint32_t *out_seqno);

static inline struct npt_context *
npt_context_from_dispatch(struct npt_dispatch_context *dispatch)
{
   return dispatch->data;
}

/* Object handle table.  Without it a compromised guest could pass an
 * arbitrary uint64_t and have the host deref it as a COM pointer. */

/* Safe with obj == NULL or type == 0.  Re-register with the same
 * type is a no-op; type upgrade (parent -> child) is allowed;
 * unrelated types log and keep the existing entry. */
void
npt_context_register_object(struct npt_context *ctx,
                            uint64_t id,
                            void *obj,
                            npt_object_type type);

void
npt_context_unregister_object(struct npt_context *ctx, uint64_t id);

/* Whether the registered object is compatible with `want`, with none of
 * npt_context_lookup_object's miss diagnostics: an id of another type is
 * an expected answer here, not a violation. */
bool
npt_context_object_is(struct npt_context *ctx, uint64_t id,
                      npt_object_type want);

/* Returns the pointer when the registered type is compatible with
 * `expected` (exact match, or either is an ancestor of the other;
 * IUNKNOWN matches anything).  On miss / type-mismatch logs the
 * violation and returns NULL; non-permissive lookups also mark the
 * decoder fatal so the ring tears down.  IUNKNOWN-expected lookups
 * are permissive: a missing id returns NULL silently to accommodate
 * the COM_RELEASE-vs-Create race. */
void *
npt_context_lookup_object(struct npt_context *ctx,
                          struct npt_cs_decoder *dec,
                          uint64_t id,
                          npt_object_type expected);

/* COM_RELEASE coordination: drop the feedback entry (if any), unmap
 * the object_table entry, then drop the host-library ref via
 * IUnknown::Release.  Stray RELEASE on an unregistered id is silent. */
void
npt_context_release_object(struct npt_context *ctx, uint64_t guest_id);

/* COM_QUERY_INTERFACE: resolve src_guest_id, call QI for riid, and on
 * success register the returned interface under new_guest_id.
 * Returns the host HRESULT (or NPT_E_NOINTERFACE if src_guest_id is
 * not registered). */
HRESULT
npt_context_query_interface(struct npt_context *ctx,
                            uint64_t src_guest_id,
                            const GUID *riid,
                            uint64_t new_guest_id);

#endif /* NPT_CONTEXT_H */
