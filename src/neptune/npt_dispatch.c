/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_dispatch.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "npt_com.h"
#include "npt_context.h"
#include "npt_cs.h"
#include "npt_event.h"
#include "npt_feedback.h"
#include "npt_heap12.h"
#include "npt_resource.h"
#include "npt_ring.h"
#include "npt_shared.h"
#include "npt_transport_defs.h"
#include "neptune-protocol/npt_protocol_host_dispatch.h"

/* True when the decoder belongs to a ring, not the context.
 * Transport commands that mutate ring state must reject ring-origin
 * calls (self-wait deadlock or nonsense semantics). */
static inline bool
npt_dispatch_is_ring_dispatch(const struct npt_context *ctx,
                              const struct npt_dispatch_context *dispatch)
{
   return dispatch->decoder != &ctx->decoder;
}

static void
npt_dispatch_set_reply_stream(struct npt_context *ctx,
                              struct npt_cs_decoder *dec,
                              struct npt_cs_encoder *enc)
{
   struct npt_cmd_set_reply_stream cmd;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.res_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   struct npt_resource *res = npt_context_get_resource(ctx, cmd.res_id);
   if (!res) {
      npt_log("set_reply_stream: invalid resource %u", cmd.res_id);
      npt_cs_decoder_set_fatal(dec);
      return;
   }

   npt_cs_encoder_set_stream(enc, res, cmd.offset, cmd.size);
}

/* Reject ring-origin: a ring creating another ring or rebinding its
 * own reply stream would deadlock or corrupt state. */
static bool
npt_require_context_origin(struct npt_context *ctx,
                           struct npt_dispatch_context *dispatch,
                           struct npt_cs_decoder *dec,
                           const char *cmd_name)
{
   if (unlikely(npt_dispatch_is_ring_dispatch(ctx, dispatch))) {
      npt_log("%s must not originate from a ring", cmd_name);
      npt_cs_decoder_set_fatal(dec);
      return false;
   }
   return true;
}

static void
npt_dispatch_create_ring(struct npt_context *ctx,
                         struct npt_cs_decoder *dec,
                         UNUSED struct npt_cs_encoder *enc)
{
   struct npt_cmd_create_ring cmd;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.ring_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   if (!npt_ring_create_from_cmd(ctx, &cmd))
      npt_cs_decoder_set_fatal(dec);
}

static void
npt_dispatch_destroy_ring(struct npt_context *ctx,
                          struct npt_cs_decoder *dec,
                          UNUSED struct npt_cs_encoder *enc)
{
   struct npt_cmd_destroy_ring cmd;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.ring_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   if (!npt_ring_destroy_by_id(ctx, cmd.ring_id))
      npt_cs_decoder_set_fatal(dec);
}

static void
npt_dispatch_notify_ring(struct npt_context *ctx,
                         struct npt_cs_decoder *dec,
                         UNUSED struct npt_cs_encoder *enc)
{
   struct npt_cmd_notify_ring cmd;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.ring_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   /* Miss is non-fatal: the guest can legitimately notify a ring
    * that raced its own DESTROY. */
   struct npt_ring *ring = npt_ring_find_by_id(ctx, cmd.ring_id);
   if (ring)
      npt_ring_notify(ring);
   else
      npt_log("notify_ring: ring %" PRIu64 " not found", cmd.ring_id);
}

static void
npt_dispatch_write_ring_extra(struct npt_context *ctx,
                              struct npt_cs_decoder *dec,
                              UNUSED struct npt_cs_encoder *enc)
{
   struct npt_cmd_write_ring_extra cmd;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.ring_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   /* Bad offset is fatal; ring-not-found is non-fatal. */
   struct npt_ring *ring = npt_ring_find_by_id(ctx, cmd.ring_id);
   if (!ring) {
      npt_log("write_ring_extra: ring %" PRIu64 " not found", cmd.ring_id);
      return;
   }
   if (!npt_ring_write_extra(ring, cmd.offset, cmd.value)) {
      npt_log("write_ring_extra: invalid offset %u", cmd.offset);
      npt_cs_decoder_set_fatal(dec);
   }
}

static void
npt_dispatch_com_release(struct npt_context *ctx,
                         struct npt_cs_decoder *dec,
                         const struct npt_command_header *header)
{
   const uint32_t payload = header->cmd_size - sizeof(*header);
   const uint32_t count = payload / sizeof(struct npt_cmd_com_release_wait);
   struct npt_cmd_com_release_wait stack_wait[32];
   struct npt_cmd_com_release_wait *wait = stack_wait;
   if (count > ARRAY_SIZE(stack_wait)) {
      wait = malloc(count * sizeof(*wait));
      if (!wait) {
         npt_cs_decoder_set_fatal(dec);
         return;
      }
   }
   if (count)
      npt_cs_decoder_read(dec, count * sizeof(*wait), wait,
                          count * sizeof(*wait));
   if (!npt_cs_decoder_get_fatal(dec))
      npt_context_release_object_ordered(ctx, header->object_id, wait, count);
   if (wait != stack_wait)
      free(wait);
}

static void
npt_dispatch_resource_update(struct npt_context *ctx,
                             struct npt_cs_decoder *dec,
                             UNUSED struct npt_cs_encoder *enc,
                             const struct npt_command_header *header)
{
   struct npt_cmd_resource_update cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.subresource,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   if (cmd.byte_size > NPT_MAX_RESOURCE_UPDATE_BYTES) {
      npt_log("resource_update: byte_size %u exceeds cap %u",
              cmd.byte_size, NPT_MAX_RESOURCE_UPDATE_BYTES);
      npt_cs_decoder_set_fatal(dec);
      return;
   }

   /* Zero-copy: SHM payload is contiguous so we skip the temp-pool
    * memcpy on the hot path.  Fall back to alloc_temp if the payload
    * straddles the stream end (shouldn't happen with contiguous SHM). */
   const uint32_t payload_aligned = (cmd.byte_size + 7u) & ~7u;
   void *data = npt_cs_decoder_get_blob_storage(dec, payload_aligned);
   if (likely(data)) {
      dec->cur += payload_aligned;
   } else {
      data = npt_cs_decoder_alloc_temp(dec, payload_aligned);
      if (!data) return;
      npt_cs_decoder_read(dec, payload_aligned, data, cmd.byte_size);
      if (npt_cs_decoder_get_fatal(dec))
         return;
   }

   npt_resource_update(ctx, header->object_id, cmd.subresource,
                       cmd.row_pitch, cmd.depth_pitch, cmd.byte_size,
                       !!cmd.has_box,
                       cmd.box_left, cmd.box_top, cmd.box_front,
                       cmd.box_right, cmd.box_bottom, cmd.box_back,
                       data);
}

static void
npt_dispatch_register_event(struct npt_context *ctx,
                            struct npt_cs_decoder *dec,
                            UNUSED struct npt_cs_encoder *enc,
                            const struct npt_command_header *header)
{
   struct npt_cmd_register_event cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.event_token, sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;
   npt_event_register(ctx, cmd.event_token);
}

static void
npt_dispatch_gate_wait(struct npt_context *ctx,
                       struct npt_cs_decoder *dec,
                       struct npt_cs_encoder *enc,
                       const struct npt_command_header *header)
{
   struct npt_cmd_gate_wait cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.fence_id, sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   /* Type-checked rather than IUnknown-permissive (which matches any
    * registered object): the gate calls this pointer through fixed
    * ID3D12Fence vtable slots, so any other type would be an
    * out-of-contract indirect call inside the shared render server. */
   void *fence = npt_context_lookup_object(ctx, dec, cmd.fence_id,
                                           NPT_OBJECT_TYPE_ID3D12FENCE);
   bool ok = fence &&
      npt_event_gate_wait(ctx, fence, cmd.value, cmd.ring_idx);
   if (!ok)
      npt_event_drain_parked_ring(ctx, cmd.ring_idx);

   struct npt_cmd_gate_wait_reply reply = { 0 };
   reply.header.cmd_type = header->cmd_type;
   reply.header.cmd_return = ok ? 0u : (uint32_t)(int32_t)NPT_E_FAIL;
   if (header->cmd_flags & NPT_CMD_FLAG_REPLY) {
      if (npt_cs_encoder_acquire(enc)) {
         npt_cs_encoder_write(enc, sizeof(reply), &reply, sizeof(reply));
         npt_cs_encoder_release(enc);
      }
   }
}

static void
npt_dispatch_arm_event_fence(struct npt_context *ctx,
                             struct npt_cs_decoder *dec,
                             struct npt_cs_encoder *enc,
                             const struct npt_command_header *header)
{
   struct npt_cmd_arm_event_fence cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.event_token, sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   bool ok = npt_event_arm(ctx, cmd.event_token, cmd.ring_idx, cmd.flags);
   if (!ok)
      npt_event_drain_parked_ring(ctx, cmd.ring_idx);

   struct npt_cmd_arm_event_fence_reply reply = { 0 };
   reply.header.cmd_type = header->cmd_type;
   reply.header.cmd_return = ok ? 0u : (uint32_t)(int32_t)NPT_E_FAIL;
   if (header->cmd_flags & NPT_CMD_FLAG_REPLY) {
      if (npt_cs_encoder_acquire(enc)) {
         npt_cs_encoder_write(enc, sizeof(reply), &reply, sizeof(reply));
         npt_cs_encoder_release(enc);
      }
   }
}

static void
npt_dispatch_release_event(struct npt_context *ctx,
                           struct npt_cs_decoder *dec,
                           UNUSED struct npt_cs_encoder *enc,
                           const struct npt_command_header *header)
{
   struct npt_cmd_release_event cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.event_token, sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;
   npt_event_release(ctx, cmd.event_token);
}

static void
npt_dispatch_com_query_interface(struct npt_context *ctx,
                                 struct npt_cs_decoder *dec,
                                 struct npt_cs_encoder *enc,
                                 const struct npt_command_header *header)
{
   struct npt_cmd_com_query_interface cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.iid, sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   const HRESULT hr = npt_context_query_interface(
      ctx, header->object_id, &cmd.iid, cmd.guest_id);

   struct npt_cmd_com_query_interface_reply reply;
   memset(&reply, 0, sizeof(reply));
   reply.header.cmd_type = header->cmd_type;
   reply.header.cmd_return = (uint32_t)hr;

   if (header->cmd_flags & NPT_CMD_FLAG_REPLY) {
      if (npt_cs_encoder_acquire(enc)) {
         npt_cs_encoder_write(enc, sizeof(reply), &reply, sizeof(reply));
         npt_cs_encoder_release(enc);
      }
   }
}

static void
npt_dispatch_map_resource(struct npt_context *ctx,
                          struct npt_cs_decoder *dec,
                          struct npt_cs_encoder *enc,
                          const struct npt_command_header *header)
{
   struct npt_cmd_map_resource cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.context_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   struct npt_cmd_map_resource_reply reply;
   memset(&reply, 0, sizeof(reply));
   reply.header.cmd_type = header->cmd_type;
   reply.header.cmd_return = (uint32_t)npt_resource_map(
      ctx, cmd.context_id, cmd.resource_id, cmd.subresource,
      cmd.access_flags, cmd.api_map_flags, cmd.shmem_res_id,
      cmd.read_range_begin, cmd.read_range_end,
      cmd.byte_size, cmd.mip_height, cmd.mip_depth,
      cmd.shmem_offset,
      &reply.row_pitch, &reply.depth_pitch, &reply.mapped_size);

   if (header->cmd_flags & NPT_CMD_FLAG_REPLY) {
      if (npt_cs_encoder_acquire(enc)) {
         npt_cs_encoder_write(enc, sizeof(reply), &reply, sizeof(reply));
         npt_cs_encoder_release(enc);
      }
   }
}

static void
npt_dispatch_unmap_resource(struct npt_context *ctx,
                            struct npt_cs_decoder *dec,
                            struct npt_cs_encoder *enc,
                            const struct npt_command_header *header)
{
   struct npt_cmd_unmap_resource cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.context_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   const HRESULT hr = npt_resource_unmap(ctx, cmd.context_id,
                                         cmd.resource_id, cmd.subresource,
                                         cmd.shmem_res_id, cmd.shmem_offset,
                                         cmd.byte_size, cmd.access_flags,
                                         cmd.written_range_begin,
                                         cmd.written_range_end);

   struct npt_cmd_unmap_resource_reply reply;
   memset(&reply, 0, sizeof(reply));
   reply.header.cmd_type = header->cmd_type;
   reply.header.cmd_return = (uint32_t)hr;

   if (header->cmd_flags & NPT_CMD_FLAG_REPLY) {
      if (npt_cs_encoder_acquire(enc)) {
         npt_cs_encoder_write(enc, sizeof(reply), &reply, sizeof(reply));
         npt_cs_encoder_release(enc);
      }
   }
}

/* Re-point the decoder at a SHM-blob chunk, dispatch the inlined
 * commands, restore the outer window. */
static void
npt_dispatch_execute_command_stream(struct npt_context *ctx,
                                    struct npt_dispatch_context *dispatch,
                                    struct npt_cs_decoder *dec,
                                    struct npt_cs_encoder *enc,
                                    const struct npt_command_header *header)
{
   (void)header;

   struct npt_cmd_execute_command_stream cmd;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.res_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   /* One level of nesting only: re-entry would corrupt saved_state
    * and recurse unboundedly. */
   if (unlikely(npt_cs_decoder_has_saved_state(dec))) {
      npt_log("execute_command_stream: nested execution is not allowed");
      npt_cs_decoder_set_fatal(dec);
      return;
   }

   struct npt_resource *res = npt_context_get_resource(ctx, cmd.res_id);
   if (!res || res->fd_type != VIRGL_RESOURCE_FD_SHM ||
       cmd.size > res->size || cmd.offset > res->size - cmd.size) {
      npt_log("execute_command_stream: invalid resource/range "
              "res=%u offset=%u size=%u", cmd.res_id, cmd.offset, cmd.size);
      npt_cs_decoder_set_fatal(dec);
      return;
   }

   npt_cs_decoder_save_state(dec);

   const uint8_t *bytes = (const uint8_t *)res->u.data + cmd.offset;
   npt_cs_decoder_set_buffer_stream(dec, bytes, cmd.size);

   while (npt_cs_decoder_has_command(dec)) {
      if (!npt_context_dispatch_one_command(ctx, dispatch, dec, enc))
         break;
   }

   npt_cs_decoder_restore_state(dec);
}

static void
npt_dispatch_wait_ring_seqno(struct npt_context *ctx,
                             struct npt_cs_decoder *dec,
                             const struct npt_command_header *header)
{
   struct npt_cmd_wait_ring_seqno cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.ring_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   struct npt_ring *ring = npt_ring_find_by_id(ctx, cmd.ring_id);
   if (!ring) {
      npt_log("WAIT_RING_SEQNO: unknown ring %" PRIu64, cmd.ring_id);
      npt_cs_decoder_set_fatal(dec);
      return;
   }

   /* Wake the ring so it runs the unreachability check; without
    * this, a bad seqno would block forever. */
   npt_ring_notify(ring);

   if (!npt_context_wait_ring_seqno(ctx, ring, cmd.seqno))
      npt_cs_decoder_set_fatal(dec);
}

/* Ring-origin only (checked by the caller). */
static void
npt_dispatch_wait_peer_ring(struct npt_context *ctx,
                            struct npt_dispatch_context *dispatch,
                            struct npt_cs_decoder *dec,
                            const struct npt_command_header *header)
{
   struct npt_cmd_wait_peer_ring cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.ring_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   struct npt_ring *self = npt_ring_from_dispatch(dispatch);
   if (unlikely(cmd.ring_id == self->id)) {
      npt_log("WAIT_PEER: ring %" PRIu64 " waits on itself", self->id);
      npt_cs_decoder_set_fatal(dec);
      return;
   }

   npt_ring_wait_peer_seqno(ctx, self, cmd.ring_id, cmd.seqno);
}

static void
npt_dispatch_submit_virtqueue_seqno(struct npt_context *ctx,
                                    struct npt_cs_decoder *dec,
                                    const struct npt_command_header *header)
{
   struct npt_cmd_submit_virtqueue_seqno cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.ring_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   struct npt_ring *ring = npt_ring_find_by_id(ctx, cmd.ring_id);
   if (!ring) {
      npt_log("SUBMIT_VIRTQUEUE_SEQNO: unknown ring %" PRIu64, cmd.ring_id);
      npt_cs_decoder_set_fatal(dec);
      return;
   }

   npt_ring_store_virtqueue_seqno(ring, cmd.seqno);
}

static void
npt_dispatch_wait_virtqueue_seqno(struct npt_context *ctx,
                                  struct npt_cs_decoder *dec,
                                  const struct npt_command_header *header)
{
   struct npt_cmd_wait_virtqueue_seqno cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header), &cmd.ring_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   struct npt_ring *ring = npt_ring_find_by_id(ctx, cmd.ring_id);
   if (!ring) {
      npt_log("WAIT_VIRTQUEUE_SEQNO: unknown ring %" PRIu64, cmd.ring_id);
      npt_cs_decoder_set_fatal(dec);
      return;
   }

   if (!npt_ring_wait_virtqueue_seqno(ring, cmd.seqno))
      npt_cs_decoder_set_fatal(dec);
}

/* Per-subgroup dispatchers.  Each returns true iff `method` matched a
 * known command in the subgroup; false propagates to the top-level
 * dispatcher, which logs and sets fatal. */

static bool
npt_dispatch_subgroup_core(struct npt_context *ctx,
                           struct npt_cs_decoder *dec,
                           struct npt_cs_encoder *enc,
                           uint32_t method)
{
   switch (method) {
   case NPT_TRANSPORT_CORE_SET_REPLY_STREAM:
      /* No origin guard: the guest sends this over the ring before
       * each sync command to keep the window change and the using
       * command in order. */
      npt_dispatch_set_reply_stream(ctx, dec, enc);
      return true;
   default:
      return false;
   }
}

static bool
npt_dispatch_subgroup_ring(struct npt_context *ctx,
                           struct npt_dispatch_context *dispatch,
                           struct npt_cs_decoder *dec,
                           struct npt_cs_encoder *enc,
                           const struct npt_command_header *header,
                           uint32_t method)
{
   const bool is_ring = npt_dispatch_is_ring_dispatch(ctx, dispatch);

   switch (method) {
   case NPT_TRANSPORT_RING_CREATE:
      if (!npt_require_context_origin(ctx, dispatch, dec, "RING_CREATE"))
         return true;
      npt_dispatch_create_ring(ctx, dec, enc);
      return true;
   case NPT_TRANSPORT_RING_DESTROY:
      if (!npt_require_context_origin(ctx, dispatch, dec, "RING_DESTROY"))
         return true;
      npt_dispatch_destroy_ring(ctx, dec, enc);
      return true;
   case NPT_TRANSPORT_RING_NOTIFY:
      if (!npt_require_context_origin(ctx, dispatch, dec, "RING_NOTIFY"))
         return true;
      npt_dispatch_notify_ring(ctx, dec, enc);
      return true;
   case NPT_TRANSPORT_RING_WRITE_EXTRA:
      if (!npt_require_context_origin(ctx, dispatch, dec, "RING_WRITE_EXTRA"))
         return true;
      npt_dispatch_write_ring_extra(ctx, dec, enc);
      return true;
   case NPT_TRANSPORT_RING_WAIT_SEQNO:
      /* Self-wait on the ring thread would deadlock — context-only. */
      if (unlikely(is_ring)) {
         npt_log("WAIT_RING_SEQNO on ring dispatch would deadlock");
         npt_cs_decoder_set_fatal(dec);
         return true;
      }
      npt_dispatch_wait_ring_seqno(ctx, dec, header);
      return true;
   case NPT_TRANSPORT_RING_SUBMIT_VQ_SEQNO:
      /* A ring thread can't publish the seqno it's blocked on. */
      if (unlikely(is_ring)) {
         npt_log("SUBMIT_VIRTQUEUE_SEQNO must not originate from the ring");
         npt_cs_decoder_set_fatal(dec);
         return true;
      }
      npt_dispatch_submit_virtqueue_seqno(ctx, dec, header);
      return true;
   case NPT_TRANSPORT_RING_WAIT_VQ_SEQNO:
      /* Ring-only: the context thread can't observe ring progress. */
      if (unlikely(!is_ring)) {
         npt_log("WAIT_VIRTQUEUE_SEQNO must originate from the ring");
         npt_cs_decoder_set_fatal(dec);
         return true;
      }
      npt_dispatch_wait_virtqueue_seqno(ctx, dec, header);
      return true;
   case NPT_TRANSPORT_RING_WAIT_PEER:
      /* Ring-only: blocking the context thread stalls the whole guest. */
      if (unlikely(!is_ring)) {
         npt_log("WAIT_PEER must originate from a ring");
         npt_cs_decoder_set_fatal(dec);
         return true;
      }
      npt_dispatch_wait_peer_ring(ctx, dispatch, dec, header);
      return true;
   default:
      return false;
   }
}

static bool
npt_dispatch_subgroup_com(struct npt_context *ctx,
                          struct npt_cs_decoder *dec,
                          struct npt_cs_encoder *enc,
                          const struct npt_command_header *header,
                          uint32_t method)
{
   switch (method) {
   case NPT_TRANSPORT_COM_RELEASE:
      npt_dispatch_com_release(ctx, dec, header);
      return true;
   case NPT_TRANSPORT_COM_QUERY_INTERFACE:
      npt_dispatch_com_query_interface(ctx, dec, enc, header);
      return true;
   default:
      return false;
   }
}

static bool
npt_dispatch_subgroup_resource(struct npt_context *ctx,
                               struct npt_dispatch_context *dispatch,
                               struct npt_cs_decoder *dec,
                               struct npt_cs_encoder *enc,
                               const struct npt_command_header *header,
                               uint32_t method)
{
   switch (method) {
   case NPT_TRANSPORT_RESOURCE_UPDATE:
      npt_dispatch_resource_update(ctx, dec, enc, header);
      return true;
   case NPT_TRANSPORT_RESOURCE_MAP:
      npt_dispatch_map_resource(ctx, dec, enc, header);
      return true;
   case NPT_TRANSPORT_RESOURCE_UNMAP:
      npt_dispatch_unmap_resource(ctx, dec, enc, header);
      return true;
   case NPT_TRANSPORT_RESOURCE_EXECUTE_CMD_STREAM:
      npt_dispatch_execute_command_stream(ctx, dispatch, dec, enc, header);
      return true;
   case NPT_TRANSPORT_RESOURCE_CREATE_HEAP_FROM_SHMEM:
      npt_dispatch_create_heap_from_shmem(ctx, dec, enc, header);
      return true;
   default:
      return false;
   }
}

static void
npt_dispatch_shared_export_blob(struct npt_context *ctx,
                                struct npt_cs_decoder *dec,
                                struct npt_cs_encoder *enc,
                                const struct npt_command_header *header)
{
   struct npt_cmd_shared_export_blob cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.blob_id, sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   HRESULT hr = npt_shared_export_blob(ctx, header->object_id, cmd.blob_id,
                                       cmd.data_res_id, cmd.data_off);

   if (cmd.header.cmd_flags & NPT_CMD_FLAG_REPLY) {
      struct npt_cmd_shared_export_blob_reply reply;
      memset(&reply, 0, sizeof(reply));
      reply.header.cmd_type = header->cmd_type;
      reply.header.cmd_return = (uint32_t)(int32_t)hr;
      if (npt_cs_encoder_acquire(enc)) {
         npt_cs_encoder_write(enc, sizeof(reply), &reply, sizeof(reply));
         npt_cs_encoder_release(enc);
      }
   }
}

static void
npt_dispatch_shared_open_res(struct npt_context *ctx,
                             struct npt_cs_decoder *dec,
                             struct npt_cs_encoder *enc,
                             const struct npt_command_header *header)
{
   struct npt_cmd_shared_open_res cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.mint_object_id, sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   HRESULT hr = npt_shared_open_res(ctx, header->object_id, &cmd);

   struct npt_cmd_shared_open_res_reply reply;
   memset(&reply, 0, sizeof(reply));
   reply.header.cmd_type = header->cmd_type;
   reply.header.cmd_return = (uint32_t)(int32_t)hr;
   if (npt_cs_encoder_acquire(enc)) {
      npt_cs_encoder_write(enc, sizeof(reply), &reply, sizeof(reply));
      npt_cs_encoder_release(enc);
   }
}

static bool
npt_dispatch_subgroup_shared(struct npt_context *ctx,
                             struct npt_cs_decoder *dec,
                             struct npt_cs_encoder *enc,
                             const struct npt_command_header *header,
                             uint32_t method)
{
   switch (method) {
   case NPT_TRANSPORT_SHARED_EXPORT_BLOB:
      npt_dispatch_shared_export_blob(ctx, dec, enc, header);
      return true;
   case NPT_TRANSPORT_SHARED_OPEN_RES:
      npt_dispatch_shared_open_res(ctx, dec, enc, header);
      return true;
   default:
      return false;
   }
}

static bool
npt_dispatch_subgroup_event(struct npt_context *ctx,
                            struct npt_cs_decoder *dec,
                            struct npt_cs_encoder *enc,
                            const struct npt_command_header *header,
                            uint32_t method)
{
   switch (method) {
   case NPT_TRANSPORT_EVENT_REGISTER:
      npt_dispatch_register_event(ctx, dec, enc, header);
      return true;
   case NPT_TRANSPORT_EVENT_ARM_FENCE:
      npt_dispatch_arm_event_fence(ctx, dec, enc, header);
      return true;
   case NPT_TRANSPORT_EVENT_GATE_WAIT:
      npt_dispatch_gate_wait(ctx, dec, enc, header);
      return true;
   case NPT_TRANSPORT_EVENT_RELEASE:
      npt_dispatch_release_event(ctx, dec, enc, header);
      return true;
   default:
      return false;
   }
}

static void
npt_dispatch_register_query_feedback(struct npt_context *ctx,
                                     struct npt_cs_decoder *dec,
                                     const struct npt_command_header *header)
{
   struct npt_cmd_register_query_feedback cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.fb_res_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;
   npt_feedback_query_register(ctx, header->object_id,
                               cmd.fb_res_id, cmd.fb_offset,
                               cmd.query_data_size);
}

static void
npt_dispatch_unregister_query_feedback(struct npt_context *ctx,
                                       UNUSED struct npt_cs_decoder *dec,
                                       const struct npt_command_header *header)
{
   npt_feedback_unregister(ctx, header->object_id);
}

static void
npt_dispatch_register_fence_feedback(struct npt_context *ctx,
                                     struct npt_cs_decoder *dec,
                                     const struct npt_command_header *header)
{
   struct npt_cmd_register_fence_feedback cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.fb_res_id,
                       sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;
   npt_feedback_fence_register(ctx, header->object_id,
                               cmd.fb_res_id, cmd.fb_offset,
                               cmd.fence_api);
}

static bool
npt_dispatch_subgroup_feedback(struct npt_context *ctx,
                               struct npt_cs_decoder *dec,
                               const struct npt_command_header *header,
                               uint32_t method)
{
   switch (method) {
   case NPT_TRANSPORT_FEEDBACK_REGISTER_QUERY:
      npt_dispatch_register_query_feedback(ctx, dec, header);
      return true;
   case NPT_TRANSPORT_FEEDBACK_UNREGISTER_QUERY:
      npt_dispatch_unregister_query_feedback(ctx, dec, header);
      return true;
   case NPT_TRANSPORT_FEEDBACK_REGISTER_FENCE:
      npt_dispatch_register_fence_feedback(ctx, dec, header);
      return true;
   default:
      return false;
   }
}

bool
npt_transport_dispatch(struct npt_context *ctx,
                       struct npt_dispatch_context *dispatch,
                       struct npt_cs_decoder *dec,
                       struct npt_cs_encoder *enc,
                       const struct npt_command_header *header)
{
   const uint32_t subgroup = npt_transport_subgroup(header);
   const uint32_t method   = npt_transport_method(header);

   bool matched = false;
   switch (subgroup) {
   case NPT_TRANSPORT_SUBGROUP_CORE:
      matched = npt_dispatch_subgroup_core(ctx, dec, enc, method);
      break;
   case NPT_TRANSPORT_SUBGROUP_RING:
      matched = npt_dispatch_subgroup_ring(ctx, dispatch, dec, enc, header,
                                           method);
      break;
   case NPT_TRANSPORT_SUBGROUP_COM:
      matched = npt_dispatch_subgroup_com(ctx, dec, enc, header, method);
      break;
   case NPT_TRANSPORT_SUBGROUP_RESOURCE:
      matched = npt_dispatch_subgroup_resource(ctx, dispatch, dec, enc,
                                               header, method);
      break;
   case NPT_TRANSPORT_SUBGROUP_SHARED:
      matched = npt_dispatch_subgroup_shared(ctx, dec, enc, header, method);
      break;
   case NPT_TRANSPORT_SUBGROUP_EVENT:
      matched = npt_dispatch_subgroup_event(ctx, dec, enc, header, method);
      break;
   case NPT_TRANSPORT_SUBGROUP_FEEDBACK:
      matched = npt_dispatch_subgroup_feedback(ctx, dec, header, method);
      break;
   default:
      break;
   }

   if (!matched) {
      npt_log("unknown transport command: subgroup=%u method=%u",
              subgroup, method);
      npt_cs_decoder_set_fatal(dec);
      return false;
   }
   return true;
}
