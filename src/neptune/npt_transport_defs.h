/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Transport (group 0) command numbers and structs.  Group 0 is not
 * covered by the protocol generator; the wire format reuses
 * npt_command_header and bodies are cast-directly C structs.
 *
 * cmd_type layout for group 0: bits 23:16 = subgroup id, bits 15:0 =
 * method id within the subgroup. */

#ifndef NPT_TRANSPORT_DEFS_H
#define NPT_TRANSPORT_DEFS_H

#include "neptune-protocol/npt_protocol_defs.h"

#define NPT_TRANSPORT_GROUP 0u

/* Subgroup ids -- bits 23:16 of cmd_type. */
#define NPT_TRANSPORT_SUBGROUP_CORE     0u  /* reply-stream setup           */
#define NPT_TRANSPORT_SUBGROUP_RING     1u  /* ring lifecycle + sync        */
#define NPT_TRANSPORT_SUBGROUP_COM      2u  /* IUnknown lifetime + QI       */
#define NPT_TRANSPORT_SUBGROUP_RESOURCE 3u  /* Map/Unmap/Update/cmd-stream  */
#define NPT_TRANSPORT_SUBGROUP_SHARED   4u  /* shared / presentable textures*/
#define NPT_TRANSPORT_SUBGROUP_EVENT    5u  /* Win32 event bridge           */
#define NPT_TRANSPORT_SUBGROUP_FEEDBACK 6u  /* shmem feedback (query, fence)*/

#define NPT_TRANSPORT_CMD_TYPE(subgroup, method) \
   NPT_CMD_TYPE_TOPLEVEL(NPT_TRANSPORT_GROUP, \
      (((uint32_t)(subgroup) & 0xFFu) << 16) | \
      ((uint32_t)(method) & 0xFFFFu))

static inline uint32_t
npt_transport_subgroup(const struct npt_command_header *hdr)
{
   return (hdr->cmd_type >> 16) & 0xFFu;
}

static inline uint32_t
npt_transport_method(const struct npt_command_header *hdr)
{
   return hdr->cmd_type & 0xFFFFu;
}

/* ====================================================================== */
/* CORE                                                                   */
/* ====================================================================== */

#define NPT_TRANSPORT_CORE_SET_REPLY_STREAM 0u

struct npt_cmd_set_reply_stream {
   struct npt_command_header header;
   uint32_t res_id;
   uint32_t offset;
   uint32_t size;
   uint32_t pad;
};

/* ====================================================================== */
/* RING                                                                   */
/* ====================================================================== */

#define NPT_TRANSPORT_RING_CREATE          0u
#define NPT_TRANSPORT_RING_DESTROY         1u
#define NPT_TRANSPORT_RING_NOTIFY          2u
#define NPT_TRANSPORT_RING_WRITE_EXTRA     3u
#define NPT_TRANSPORT_RING_WAIT_SEQNO      4u
#define NPT_TRANSPORT_RING_SUBMIT_VQ_SEQNO 5u
#define NPT_TRANSPORT_RING_WAIT_VQ_SEQNO   6u
#define NPT_TRANSPORT_RING_WAIT_PEER       7u

/* Ring status bits (host writes, guest reads).  ALIVE is OR-set by
 * the ring-monitor thread on opted-in rings; the guest watchdog
 * clears and re-checks to detect a wedged host. */
#define NPT_RING_STATUS_IDLE_BIT  (1u << 0)
#define NPT_RING_STATUS_FATAL_BIT (1u << 1)
#define NPT_RING_STATUS_ALIVE_BIT (1u << 2)

struct npt_cmd_create_ring {
   struct npt_command_header header;
   uint64_t ring_id;
   uint32_t res_id;
   uint32_t head_offset;
   uint32_t tail_offset;
   uint32_t status_offset;
   uint32_t buffer_offset;
   uint32_t buffer_size;
   uint32_t extra_offset;
   uint32_t extra_size;
   uint64_t idle_timeout;
   /* 0 = no monitor; nonzero starts (or tightens) a per-context
    * thread that OR-sets ALIVE every period_us. */
   uint32_t monitor_report_period_us;
   /* 0 = inherited priority; nonzero asks for setpriority()
    * (best-effort, may fail without CAP_SYS_NICE). */
   uint32_t priority_valid;
   int32_t  priority;
   /* Guest-chosen byte offset within the ring blob at which the host writes the
    * workaround-flags word; 0 = the guest does not want them.  The bits are
    * defined per side against the same wire values: npt_library.h on the host,
    * npt_workaround.h on the guest. */
   uint32_t workaround_offset;
};

struct npt_cmd_destroy_ring {
   struct npt_command_header header;
   uint64_t ring_id;
};

struct npt_cmd_notify_ring {
   struct npt_command_header header;
   uint64_t ring_id;
};

struct npt_cmd_write_ring_extra {
   struct npt_command_header header;
   uint64_t ring_id;
   uint32_t offset;
   uint32_t value;
};

/* Slow-path ring-seqno wait, after the guest's shmem-head spin
 * check misses.  Must dispatch from context, not the ring itself
 * (host rejects ring-origin to prevent self-wait deadlock). */
struct npt_cmd_wait_ring_seqno {
   struct npt_command_header header;
   uint64_t ring_id;
   uint32_t seqno;
   uint32_t pad;
};

/* Ring -> ring ordering edge.  Ring-origin only: the decoding ring
 * blocks until ring_id's published decode position (its head) reaches
 * seqno, then continues.  The guest emits it only for a seqno that was
 * already published on ring_id (its tail) when the edge was written,
 * which is what keeps every such wait satisfiable and the edge graph
 * acyclic: an edge can only point at bytes that were written before
 * it.  ring_id == the decoding ring is a guest bug (self-wait). */
struct npt_cmd_wait_peer_ring {
   struct npt_command_header header;
   uint64_t ring_id;
   uint32_t seqno;
   uint32_t pad;
};

/* Context → ring ordering primitive.  A submit_cmd-originated
 * command and a ring-originated command run on different host
 * threads, so "C_ring observes C_submit's effect" needs explicit
 * synchronisation:
 *
 *   After C_submit (context):
 *      seqno = ++counter;
 *      submit_cmd(SUBMIT_VQ_SEQNO{ring, seqno});
 *   Before C_ring (ring):
 *      ring_submit(WAIT_VQ_SEQNO{ring, seqno});
 *      ring_submit(C_ring);
 *
 * SUBMIT is context-only, WAIT is ring-only. */
struct npt_cmd_submit_virtqueue_seqno {
   struct npt_command_header header;
   uint64_t ring_id;
   uint32_t seqno;
   uint32_t pad;
};

struct npt_cmd_wait_virtqueue_seqno {
   struct npt_command_header header;
   uint64_t ring_id;
   uint32_t seqno;
   uint32_t pad;
};

/* ====================================================================== */
/* COM                                                                    */
/* ====================================================================== */

#define NPT_TRANSPORT_COM_RELEASE         0u
#define NPT_TRANSPORT_COM_QUERY_INTERFACE 1u

/* Sent when the guest wrapper refcount hits zero.  object_id lives in
 * header.object_id.  The payload names, for every OTHER ring of the
 * device, the position that ring had published when the release was
 * sent: uses of the object still undecoded on those rings sit at or
 * below it, and the host runs the release only once each ring has
 * decoded past its entry.  The guest supplies these because it reads
 * its own ring tails exactly, whereas the host sees them through its
 * cache with a lag.  Entries for a ring the host no longer has are
 * satisfied.  count is derived from cmd_size. */
struct npt_cmd_com_release_wait {
   uint64_t ring_id;
   uint32_t seqno;
   uint32_t pad;
};

struct npt_cmd_com_release {
   struct npt_command_header header;
   /* Followed by (cmd_size - sizeof(header)) / sizeof(struct
    * npt_cmd_com_release_wait) entries. */
};

/* Synchronous: the caller branches on E_NOINTERFACE to decide
 * whether the wrapper stays live.  Guest pre-allocates `guest_id`;
 * on success the host registers the returned interface under it. */
struct npt_cmd_com_query_interface {
   struct npt_command_header header;
   /* header.object_id = source object's guest id */
   GUID iid;
   uint64_t guest_id;
};

struct npt_cmd_com_query_interface_reply {
   struct npt_reply_header header;
};

/* ====================================================================== */
/* RESOURCE                                                               */
/* ====================================================================== */

#define NPT_TRANSPORT_RESOURCE_UPDATE                0u
#define NPT_TRANSPORT_RESOURCE_MAP                   1u
#define NPT_TRANSPORT_RESOURCE_UNMAP                 2u
#define NPT_TRANSPORT_RESOURCE_EXECUTE_CMD_STREAM    3u
#define NPT_TRANSPORT_RESOURCE_CREATE_HEAP_FROM_SHMEM 4u

/* UpdateSubresource(1) wire path; also serves D3D11_SUBRESOURCE_DATA
 * pSysMem.  D3D12 WriteToSubresource has identical shape (same box
 * semantics, pitch fields, and payload trailer).  D3D12 dispatcher
 * calls ID3D12Resource::WriteToSubresource directly; D3D11 recovers
 * the device via ID3D11DeviceChild::GetDevice on the resource and
 * the immediate context via ID3D11Device::GetImmediateContext. */
struct npt_cmd_resource_update {
   struct npt_command_header header;
   /* header.object_id = host ID3D11Resource (D3D11) /
    *                    host ID3D12Resource (D3D12) */
   uint32_t subresource;
   uint32_t row_pitch;
   uint32_t depth_pitch;
   uint32_t byte_size;
   /* has_box == 0 ⇒ whole subresource, box_* ignored.  Buffer boxes:
    * top/front=0, bottom/back=1, (left,right) as byte offsets.
    * Texture boxes: standard six-dim. */
   uint32_t has_box;
   uint32_t box_left;
   uint32_t box_top;
   uint32_t box_front;
   uint32_t box_right;
   uint32_t box_bottom;
   uint32_t box_back;
   /* Explicit pad matches the trailing padding the header's uint64_t
    * forces on this struct.  Without it the host body decode (which
    * excludes the header) under-reads by 4 bytes versus guest sizeof. */
   uint32_t pad;
   /* followed by `byte_size` bytes of payload */
};

/* API-agnostic; the host dispatch translates to D3D11_MAP / D3D12. */
#define NPT_MAP_ACCESS_READ          0x1u
#define NPT_MAP_ACCESS_WRITE         0x2u
#define NPT_MAP_ACCESS_DISCARD       0x4u
#define NPT_MAP_ACCESS_NO_OVERWRITE  0x8u
#define NPT_MAP_ACCESS_PERSISTENT    0x10u /* D3D12: persistent map */
#define NPT_MAP_ACCESS_NO_GPU_SYNC   0x20u /* D3D12 */

/* Synchronous: the host Map can block on GPU sync; the guest
 * spin-waits on the ring head for the reply.
 * D3D11: context_id = ID3D11DeviceContext*, resource_id = ID3D11Resource*.
 * D3D12: context_id = 0, resource_id = ID3D12Resource*; read_range
 *        carries the D3D12_RANGE (0,0 = full resource). */
/* D3D12 range encoding: (begin == end == NPT_MAP_RANGE_NULL) means a
 * NULL D3D12_RANGE pointer ("may read / wrote everything"); anything
 * else is a literal {Begin, End} pair ({0,0} = empty range). */
#define NPT_MAP_RANGE_NULL (~0ull)

struct npt_cmd_map_resource {
   struct npt_command_header header;
   uint64_t context_id;       /* D3D11: ID3D11DeviceContext*; D3D12: 0 */
   uint64_t resource_id;
   uint32_t subresource;
   uint32_t access_flags;
   uint32_t api_map_flags;    /* D3D11_MAP_FLAG bitmask; D3D12: 0 */
   uint32_t shmem_res_id;
   uint64_t read_range_begin; /* D3D12: pReadRange->Begin (D3D11: ignored) */
   uint64_t read_range_end;
   /* Subresource byte count.  Host clamps memcpy to MIN(byte_size,
    * shmem size) to bound over-reads.  0 = use shmem size. */
   uint64_t byte_size;
   /* Texture-only mip-aware dims.  When both nonzero the host
    * computes READ memcpy size as RowPitch * mip_height * mip_depth
    * (clamped to shmem).  Required for non-mip-0 subresources where
    * the backend row_pitch can differ from the guest's
    * pre-allocation estimate.  Buffer callers leave both 0. */
   uint32_t mip_height;
   uint32_t mip_depth;
   /* Byte offset of the guest's slot window inside the shmem blob.
    * The map pool sub-allocates many resources from one blob, so a
    * READ prefill must land at the slot, not the blob base (a copy at
    * the base hands the caller garbage AND clobbers whichever
    * resource owns offset 0). */
   uint32_t shmem_offset;
   uint32_t pad;
};

struct npt_cmd_map_resource_reply {
   struct npt_reply_header header;
   /* header.cmd_return = HRESULT from Map */
   uint32_t row_pitch;
   uint32_t depth_pitch;
   uint32_t mapped_size;      /* 0 for persistent */
   uint32_t pad;
};

/* Synchronous: guest must not submit Draws reading this resource until
 * the host has finished copying and unmapped. */
struct npt_cmd_unmap_resource {
   struct npt_command_header header;
   uint64_t context_id;         /* D3D11: ID3D11DeviceContext*; D3D12: 0 */
   uint64_t resource_id;
   uint32_t subresource;
   uint32_t shmem_res_id;
   uint64_t written_range_begin; /* D3D12 (D3D11: ignored) */
   uint64_t written_range_end;
   uint64_t byte_size;
   /* Nonzero = rename-ring path: host runs a fresh Map + memcpy +
    * Unmap here with no prior MAP_RESOURCE.  Zero = paired with a
    * prior MAP_RESOURCE. */
   uint32_t access_flags;
   /* Byte offset within shmem_res_id; single-region callers leave 0. */
   uint32_t shmem_offset;
};

struct npt_cmd_unmap_resource_reply {
   struct npt_reply_header header;
   /* header.cmd_return = 0 on success */
};

/* Indirect submission: the guest parks an oversized command in a
 * SHM blob and writes only this reference into the ring.
 * Fire-and-forget. */
struct npt_cmd_execute_command_stream {
   struct npt_command_header header;
   uint32_t res_id;
   uint32_t offset;
   uint32_t size;
   uint32_t pad;
};

/* Synchronous.  D3D12 persistently-mapped heap path: import the guest
 * SHM blob window [shmem_offset, shmem_offset + size) as an ID3D12Heap
 * registered under mint_heap_id.  shmem_offset must be page-aligned;
 * size is 64 KiB-aligned by the guest allocator.  heap_type/heap_flags
 * are the app's original values and informational here -- the guest
 * answers GetDesc / GetHeapProperties locally.  Reply: cmd_return =
 * HRESULT. */
struct npt_cmd_create_heap_from_shmem {
   struct npt_command_header header;
   /* header.object_id = guest device id */
   uint64_t mint_heap_id;
   uint32_t shmem_res_id;
   uint32_t shmem_offset;
   uint64_t size;
   uint32_t heap_type;    /* D3D12_HEAP_TYPE (app's) */
   uint32_t heap_flags;   /* D3D12_HEAP_FLAGS (app's) */
};

struct npt_cmd_create_heap_from_shmem_reply {
   struct npt_reply_header header; /* header.cmd_return = HRESULT */
};

/* ====================================================================== */
/* SHARED                                                                 */
/* ====================================================================== */

/* Shared / presentable textures follow the Venus blob model: the
 * exporter's host texture is exported as a dmabuf and staged as a
 * pending blob under blob_id; the guest KMD claims it with
 * RESOURCE_CREATE_BLOB(HOST3D, blob_id), which binds a VM-global
 * virtio res_id to the dmabuf.  Cross-process consumers reach the
 * same storage by res_id: the guest KMD attaches the resource to the
 * consumer's context (the proxy forwards the fd into that worker) and
 * OPEN_RES imports it on the consumer's device.  fds never cross the
 * wire; texture metadata rides the WDDM allocation private data. */

#define NPT_TRANSPORT_SHARED_EXPORT_BLOB 0u
#define NPT_TRANSPORT_SHARED_OPEN_RES    1u

#define NPT_BLOB_EXPORT_MAX_PLANES 4

/* dmabuf-level export description, written by the host into the
 * exporter's shmem window and round-tripped (via guest-side WDDM
 * private data) into OPEN_RES.  No fd: the fd travels as the blob
 * resource itself. */
struct npt_blob_export_info {
   uint64_t modifier;         /* DRM format modifier of the export */
   uint64_t allocation_size;  /* exporter's memory object size */
   uint32_t plane_count;
   uint32_t texture_layout;   /* D3D11_TEXTURE_LAYOUT */
   struct {
      uint64_t offset;
      uint64_t pitch;
   } planes[NPT_BLOB_EXPORT_MAX_PLANES];
};

/* Synchronous.  Export the shared host texture in header.object_id as
 * a dmabuf and stage it as this context's pending blob under blob_id.
 * The host writes npt_blob_export_info at (data_res_id, data_off).
 * Must complete before the guest KMD's RESOURCE_CREATE_BLOB claims
 * blob_id (the guest orders this by waiting for the reply before
 * creating the WDDM allocation).  Reply: cmd_return = HRESULT. */
struct npt_cmd_shared_export_blob {
   struct npt_command_header header;
   uint64_t blob_id;
   uint32_t data_res_id;
   uint32_t data_off;
};

struct npt_cmd_shared_export_blob_reply {
   struct npt_reply_header header; /* header.cmd_return = HRESULT */
};

/* Synchronous.  Import the shared texture backed by virtio resource
 * res_id on the consumer device in header.object_id and register the
 * imported texture under mint_object_id.  The D3D11 description and
 * export info round-tripped through the exporter's WDDM allocation
 * private data.  The host waits (bounded) for res_id to arrive via
 * the proxy's attach-forwarding, which the guest KMD triggered when
 * the consumer opened the allocation.  Reply: cmd_return = HRESULT. */
struct npt_cmd_shared_open_res {
   struct npt_command_header header;
   uint64_t mint_object_id;
   uint32_t res_id;
   /* D3D11_TEXTURE2D_DESC of the exporter's texture. */
   uint32_t width;
   uint32_t height;
   uint32_t mip_levels;
   uint32_t array_size;
   uint32_t format;           /* DXGI_FORMAT */
   uint32_t sample_count;
   uint32_t usage;            /* D3D11_USAGE */
   uint32_t bind_flags;
   uint32_t cpu_access_flags;
   uint32_t misc_flags;
   /* Explicit pad: 11 uint32 fields follow the uint64 mint_object_id,
    * so export_info (uint64-first) needs 4 bytes to stay 8-aligned
    * without compiler-inserted padding. */
   uint32_t pad;
   struct npt_blob_export_info export_info;
};

struct npt_cmd_shared_open_res_reply {
   struct npt_reply_header header; /* header.cmd_return = HRESULT */
};

/* ====================================================================== */
/* EVENT                                                                  */
/* ====================================================================== */

#define NPT_TRANSPORT_EVENT_REGISTER  0u
#define NPT_TRANSPORT_EVENT_ARM_FENCE 1u
#define NPT_TRANSPORT_EVENT_RELEASE   2u
#define NPT_TRANSPORT_EVENT_GATE_WAIT 3u

/* Token = (uintptr_t) of the guest-side HANDLE. */
struct npt_cmd_register_event {
   struct npt_command_header header;
   uint64_t event_token;
};

/* Synchronous: tells the host the next submit_fence on ring_idx
 * should use this token's proxy eventfd as its sync source.  Sync
 * so the guest knows the pending-arm is installed before the
 * matching command submission. */
/* AUTO_RELEASE: no guest waiter owns this token (monitored-fence gate
 * arms).  The guest sends no REGISTER/RELEASE; the host transfers the
 * arm's proxy reference to the sync-queue entry at pairing and releases
 * it after the fence retires, so the signal handle the D3D library
 * stored stays valid until it has been written. */
#define NPT_EVENT_ARM_FLAG_AUTO_RELEASE 0x1u

struct npt_cmd_arm_event_fence {
   struct npt_command_header header;
   uint64_t event_token;
   uint32_t ring_idx;
   /* NPT_EVENT_ARM_FLAG_*.  Occupies what older guests send as zeroed
    * padding, so no flag may mean anything other than "off". */
   uint32_t flags;
};

struct npt_cmd_arm_event_fence_reply {
   struct npt_reply_header header;
};

struct npt_cmd_release_event {
   struct npt_command_header header;
   uint64_t event_token;
};

/* Synchronous: monitored-fence gate wait (D3D12 DDI path).  The host
 * arms SetEventOnCompletion(fence, value) onto ring_idx's gate event and
 * installs the pending arm; the next submit_fence on ring_idx retires
 * only once the fence truly reaches value, re-checked on every wakeup.
 *
 * The gate event is per-ring and persistent rather than per-arm because
 * the host D3D library fires completions asynchronously against a handle
 * it captured at arm time: a single-use event closed at retirement would
 * hand its identity to whatever opens next.  The wakeup object has to
 * outlive any one arm, so correctness comes from the value check and
 * never from which event fired -- which also makes another gate's fire
 * landing here harmless. */
struct npt_cmd_gate_wait {
   struct npt_command_header header;
   uint64_t fence_id; /* npt object id of the (drain) fence */
   uint64_t value;
   uint32_t ring_idx;
   uint32_t pad;
};

struct npt_cmd_gate_wait_reply {
   struct npt_reply_header header;
};

/* ====================================================================== */
/* FEEDBACK                                                               */
/* ====================================================================== */

#define NPT_TRANSPORT_FEEDBACK_REGISTER_QUERY   0u
#define NPT_TRANSPORT_FEEDBACK_UNREGISTER_QUERY 1u
#define NPT_TRANSPORT_FEEDBACK_REGISTER_FENCE   2u

/* D3D11 query feedback.  Each query/predicate/counter has a 128-byte
 * shmem slot the host writes when a Begin/End cycle's result is
 * ready, so the guest's GetData reads locally instead of
 * round-tripping.
 *
 * No dedicated wire ops: guest's Begin override stamps the slot
 * version and clears the flag locally; host's End dispatch hook
 * tags the entry pending; host's idle-loop polls GetData(NO_FLUSH)
 * and on S_OK writes result + flag=1.  Guest's GetData S_FALSEs on
 * version mismatch or flag==0. */
#define NPT_QUERY_FEEDBACK_SLOT_SIZE     128u
#define NPT_QUERY_FEEDBACK_SLOT_RESULT   120u
#define NPT_QUERY_FEEDBACK_FLAG_READY    1u

struct npt_query_feedback_slot {
   /* Packed (version << 32 | flag): one 64-bit atomic so the guest
    * observes both fields consistently.  version is monotonic per
    * Begin; flag = NPT_QUERY_FEEDBACK_FLAG_READY. */
   _Atomic uint64_t state;
   uint8_t result[NPT_QUERY_FEEDBACK_SLOT_RESULT];
};

struct npt_cmd_register_query_feedback {
   struct npt_command_header header;
   /* header.object_id = guest query id */
   uint32_t fb_res_id;
   uint32_t fb_offset;
   uint32_t query_data_size;
   uint32_t pad;
};

struct npt_cmd_unregister_query_feedback {
   struct npt_command_header header;
   /* header.object_id = guest query id */
};

struct npt_cmd_query_begin {
   struct npt_command_header header;
   uint64_t ctx_id;          /* host ID3D11DeviceContext id */
   uint64_t query_id;        /* host ID3D11Asynchronous id */
   uint32_t version;
   uint32_t pad;
};

struct npt_cmd_query_end {
   struct npt_command_header header;
   uint64_t ctx_id;
   uint64_t query_id;
};

/* Fence feedback (D3D11 + D3D12).  Each registered fence gets a slot the
 * host publishes the latest observed completed value into, so the guest
 * reads it with an acquire load instead of a wire round trip.  The store
 * is a plain snapshot rather than a monotonic CAS because a D3D12 fence
 * may legally be rewound; a monotonic reader treats the slot as a lower
 * bound.  A slot still holding its seeded initial value means no Signal
 * has been observed yet.
 *
 * Unregistration piggybacks on COM_RELEASE and runs BEFORE the
 * IUnknown::Release, so the poll never touches a freed pointer. */
#define NPT_FENCE_FEEDBACK_SLOT_SIZE  16u

#define NPT_FENCE_FEEDBACK_API_D3D11  11u
#define NPT_FENCE_FEEDBACK_API_D3D12  12u

struct npt_d3d11_fence_feedback_slot {
   _Atomic uint64_t completed_value;
   uint64_t pad;
};

struct npt_cmd_register_fence_feedback {
   struct npt_command_header header;
   /* header.object_id = guest fence id */
   uint32_t fb_res_id;
   uint32_t fb_offset;
   /* NPT_FENCE_FEEDBACK_API_* -- selects the GetCompletedValue vtable
    * the host poll calls. */
   uint32_t fence_api;
   uint32_t pad;
};

#endif /* NPT_TRANSPORT_DEFS_H */
