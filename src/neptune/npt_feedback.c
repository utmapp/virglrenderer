/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-context shared-memory feedback: substrate plus per-type
 * lifecycle (query, fence).  See npt_feedback.h for design notes.
 */

#include "npt_feedback.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "util/hash_table.h"

#include "npt_com.h"
#include "npt_common.h"
#include "npt_context.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_transport_defs.h"
#include "virgl_resource.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d11fence.h"
#include "neptune-protocol/npt_protocol_host_id3d12fence.h"

/* ================================================================== */
/* Substrate                                                           */
/* ================================================================== */

/* obj_id is uint64_t. */
static uint32_t
fb_hash_u64(const void *key)
{
   uint64_t v = *(const uint64_t *)key;
   v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ull;
   v = (v ^ (v >> 27)) * 0x94d049bb133111ebull;
   return (uint32_t)(v ^ (v >> 31));
}

static bool
fb_equal_u64(const void *a, const void *b)
{
   return *(const uint64_t *)a == *(const uint64_t *)b;
}

void
npt_feedback_init(struct npt_context *ctx)
{
   struct npt_feedback_state *st = &ctx->feedback;
   memset(st, 0, sizeof(*st));
   mtx_init(&st->mutex, mtx_plain);
   list_inithead(&st->pending);
   st->table = _mesa_hash_table_create(NULL, fb_hash_u64, fb_equal_u64);
}

void
npt_feedback_fini(struct npt_context *ctx)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table)
      return;

   mtx_lock(&st->mutex);
   hash_table_foreach(st->table, e) {
      free(e->data);
   }
   _mesa_hash_table_destroy(st->table, NULL);
   st->table = NULL;
   list_inithead(&st->pending);
   st->pending_count = 0;
   mtx_unlock(&st->mutex);
   mtx_destroy(&st->mutex);
}

void
npt_feedback_state_lock(struct npt_context *ctx)
{
   mtx_lock(&ctx->feedback.mutex);
}

void
npt_feedback_state_unlock(struct npt_context *ctx)
{
   mtx_unlock(&ctx->feedback.mutex);
}

struct npt_feedback_entry *
npt_feedback_lookup_locked(struct npt_context *ctx, uint64_t obj_id)
{
   struct npt_feedback_state *st = &ctx->feedback;
   struct hash_entry *e = _mesa_hash_table_search(st->table, &obj_id);
   return e ? e->data : NULL;
}

struct npt_feedback_entry *
npt_feedback_lookup_by_host_obj_locked(struct npt_context *ctx,
                                       void *host_obj,
                                       uint8_t type)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table || !host_obj)
      return NULL;

   hash_table_foreach(st->table, e) {
      struct npt_feedback_entry *entry = e->data;
      if (entry->type == type && entry->host_obj == host_obj)
         return entry;
   }
   return NULL;
}

void
npt_feedback_set_pending_locked(struct npt_context *ctx,
                                struct npt_feedback_entry *entry)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (entry->pending)
      return;
   list_addtail(&entry->pending_head, &st->pending);
   entry->pending = true;
   st->pending_count++;
}

void
npt_feedback_clear_pending_locked(struct npt_context *ctx,
                                  struct npt_feedback_entry *entry)
{
   struct npt_feedback_state *st = &ctx->feedback;
   if (!entry->pending)
      return;
   list_del(&entry->pending_head);
   entry->pending = false;
   st->pending_count--;
}

struct npt_feedback_entry *
npt_feedback_register_locked(struct npt_context *ctx,
                             uint64_t obj_id,
                             uint8_t type,
                             uint32_t fb_res_id,
                             uint32_t fb_offset,
                             uint32_t slot_size)
{
   if (!ctx || !obj_id || !fb_res_id || !slot_size)
      return NULL;

   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table)
      return NULL;

   mtx_lock(&st->mutex);
   struct npt_feedback_entry *entry = npt_feedback_lookup_locked(ctx, obj_id);
   if (entry) {
      /* Re-register: drop pending state and refresh slot info.
       * Clear type-specific fields so a stale resolve from the
       * previous registration can't leak through. */
      npt_feedback_clear_pending_locked(ctx, entry);
      entry->type = type;
      entry->fb_res_id = fb_res_id;
      entry->fb_offset = fb_offset;
      entry->slot_size = slot_size;
      entry->host_obj = NULL;
      entry->host_ctx = NULL;
      entry->cookie = 0;
      entry->version = 0;
      entry->target_value = 0;
      entry->published_high = 0;
      entry->persistent = false;
      entry->rewound = false;
      /* Keep mutex held: caller sets type-specific fields then calls
       * npt_feedback_state_unlock. */
      return entry;
   }

   entry = calloc(1, sizeof(*entry));
   if (!entry) {
      mtx_unlock(&st->mutex);
      npt_log("feedback: OOM registering obj 0x%" PRIx64 " type=%u",
              obj_id, (unsigned)type);
      return NULL;
   }
   entry->obj_id = obj_id;
   entry->type = type;
   entry->fb_res_id = fb_res_id;
   entry->fb_offset = fb_offset;
   entry->slot_size = slot_size;
   list_inithead(&entry->pending_head);
   _mesa_hash_table_insert(st->table, &entry->obj_id, entry);
   /* Keep mutex held: caller sets type-specific fields. */
   return entry;
}

void
npt_feedback_unregister(struct npt_context *ctx, uint64_t obj_id)
{
   if (!ctx || !obj_id)
      return;
   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table)
      return;

   mtx_lock(&st->mutex);
   struct hash_entry *e = _mesa_hash_table_search(st->table, &obj_id);
   if (e) {
      struct npt_feedback_entry *entry = e->data;
      npt_feedback_clear_pending_locked(ctx, entry);
      _mesa_hash_table_remove(st->table, e);
      free(entry);
   }
   mtx_unlock(&st->mutex);
}

void *
npt_feedback_slot_ptr(struct npt_resource *res, uint32_t offset, uint32_t size)
{
   if (!res || res->fd_type != VIRGL_RESOURCE_FD_SHM || !res->u.data)
      return NULL;
   if ((size_t)offset + size > res->size)
      return NULL;
   return res->u.data + offset;
}

/* Forward decls for the type dispatch below. */
static bool
npt_feedback_query_poll_one(struct npt_context *ctx,
                            struct npt_feedback_entry *entry);
static bool
npt_feedback_fence_poll_one(struct npt_context *ctx,
                            struct npt_feedback_entry *entry);

void
npt_feedback_poll(struct npt_context *ctx, uint32_t *skip)
{
   if (!ctx)
      return;
   /* Amortize the deadline check; see NPT_FEEDBACK_POLL_CHECK_EVERY. */
   if (*skip) {
      (*skip)--;
      return;
   }
   *skip = NPT_FEEDBACK_POLL_CHECK_EVERY - 1;

   npt_feedback_poll_interval(ctx, NPT_FEEDBACK_POLL_INTERVAL_NS);
}

void
npt_feedback_poll_interval(struct npt_context *ctx, uint64_t min_interval_ns)
{
   if (!ctx)
      return;
   struct npt_feedback_state *st = &ctx->feedback;
   if (!st->table)
      return;

   if (atomic_load_explicit((_Atomic uint32_t *)&st->pending_count,
                            memory_order_relaxed) == 0)
      return;

   const uint64_t now = npt_profile_now_ns();
   if (now - st->last_poll_ns < min_interval_ns)
      return;

   mtx_lock(&st->mutex);
   if (st->pending_count == 0) {
      st->last_poll_ns = now;
      mtx_unlock(&st->mutex);
      return;
   }

   list_for_each_entry_safe(struct npt_feedback_entry, entry,
                            &st->pending, pending_head) {
      bool remove = false;
      switch (entry->type) {
      case NPT_FEEDBACK_TYPE_QUERY:
         remove = npt_feedback_query_poll_one(ctx, entry);
         break;
      case NPT_FEEDBACK_TYPE_FENCE:
      case NPT_FEEDBACK_TYPE_FENCE12:
         remove = npt_feedback_fence_poll_one(ctx, entry);
         break;
      default:
         /* Unknown type: drop from pending so we don't spin; the
          * entry itself stays for unregister to free. */
         remove = true;
         break;
      }
      if (remove)
         npt_feedback_clear_pending_locked(ctx, entry);
   }
   const uint64_t end_ns = npt_profile_now_ns();
   st->total_poll_ns += end_ns - now;
   st->poll_count++;
   st->last_poll_ns = end_ns;
   mtx_unlock(&st->mutex);
}

/* ================================================================== */
/* Query feedback                                                      */
/* ================================================================== */

/* D3D11 has no GPU-side equivalent of vkCmdCopyQueryPoolResults, so
 * the dispatch thread polls GetData on the CPU between commands.
 * Single-threaded dispatch preserves FIFO ordering. */

/* Caller holds feedback state lock. */
static bool
npt_feedback_query_poll_one(struct npt_context *ctx,
                            struct npt_feedback_entry *entry)
{
   if (!entry->host_ctx || !entry->host_obj)
      return false;
   if (!entry->fb_res_id || !entry->cookie /* query_data_size */)
      return false;
   if (entry->cookie > NPT_QUERY_FEEDBACK_SLOT_RESULT)
      return false;

   struct npt_resource *res = npt_context_get_resource(ctx, entry->fb_res_id);
   struct npt_query_feedback_slot *slot =
      npt_feedback_slot_ptr(res, entry->fb_offset, sizeof(*slot));
   if (!slot)
      return false;

   /* PIPELINE_STATISTICS is 88 bytes; the slot caps result room. */
   uint8_t scratch[NPT_QUERY_FEEDBACK_SLOT_RESULT];
   /* D3D11_ASYNC_GETDATA_DONOTFLUSH: D3D11 forbids implicit Flush
    * from inside another method's body, and the host driver crashes
    * if it happens mid-CopySubresourceRegion. */
   PFN_ID3D11DeviceContext_GetData get_data =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_GetData,
                        npt_com_vtable(entry->host_ctx),
                        NPT_VTBL_ID3D11DeviceContext_GetData);
   HRESULT hr = get_data(entry->host_ctx, entry->host_obj,
                         scratch, entry->cookie,
                         /*D3D11_ASYNC_GETDATA_DONOTFLUSH=*/1);
   if (hr != 0 /* S_OK */)
      return false;

   memcpy(slot->result, scratch, entry->cookie);
   /* Release pairs with the guest's acquire-load on state.  Packing
    * (version, flag) into one 64-bit atomic gives an atomic snapshot. */
   const uint64_t state =
      ((uint64_t)entry->version << 32) | NPT_QUERY_FEEDBACK_FLAG_READY;
   atomic_store_explicit(&slot->state, state, memory_order_release);
   return true;
}

void
npt_feedback_query_register(struct npt_context *ctx,
                            uint64_t query_id,
                            uint32_t fb_res_id,
                            uint32_t fb_offset,
                            uint32_t query_data_size)
{
   if (!ctx || !query_id || !fb_res_id ||
       query_data_size > NPT_QUERY_FEEDBACK_SLOT_RESULT)
      return;

   struct npt_feedback_entry *entry = npt_feedback_register_locked(
      ctx, query_id, NPT_FEEDBACK_TYPE_QUERY, fb_res_id, fb_offset,
      sizeof(struct npt_query_feedback_slot));
   if (!entry)
      return;
   /* Mutex held by register_locked. */
   entry->cookie = query_data_size;
   entry->version = 0;
   entry->persistent = false;
   npt_feedback_state_unlock(ctx);
}

void
npt_feedback_query_mark_end(struct npt_context *ctx,
                            void *host_ctx,
                            void *host_query)
{
   if (!ctx || !host_query)
      return;
   if (!ctx->feedback.table)
      return;

   npt_feedback_state_lock(ctx);
   /* Linear scan: registered query counts are small in practice. */
   struct npt_feedback_entry *match = NULL;
   hash_table_foreach(ctx->feedback.table, e) {
      struct npt_feedback_entry *entry = e->data;
      if (entry->type != NPT_FEEDBACK_TYPE_QUERY)
         continue;
      if (entry->host_obj == host_query) {
         match = entry;
         break;
      }
      /* Resolve host_obj from guest_id on first encounter. */
      if (!entry->host_obj) {
         entry->host_obj = npt_context_lookup_object(
            ctx, NULL, entry->obj_id, NPT_OBJECT_TYPE_IUNKNOWN);
         if (entry->host_obj == host_query) {
            match = entry;
            break;
         }
      }
   }
   if (match) {
      if (!match->host_ctx)
         match->host_ctx = host_ctx;
      npt_feedback_set_pending_locked(ctx, match);
   }
   npt_feedback_state_unlock(ctx);

   /* Only a ring thread polls, and it may be parked; tell it a poll is
    * owed.  After the state unlock: npt_ring_notify_feedback() takes
    * ring->mutex, which the poll path nests the other way round. */
   if (match)
      npt_context_notify_rings_feedback(ctx);
}

/* ================================================================== */
/* Fence feedback                                                      */
/* ================================================================== */

/* D3D11 has no ID3D11Fence::Signal; the lifecycle hook lives on
 * ID3D11DeviceContext4::Signal and calls npt_feedback_fence_mark_signal. */

static bool
npt_feedback_type_is_fence(uint8_t type)
{
   return type == NPT_FEEDBACK_TYPE_FENCE ||
          type == NPT_FEEDBACK_TYPE_FENCE12;
}

void
npt_feedback_fence_register(struct npt_context *ctx,
                            uint64_t fence_id,
                            uint32_t fb_res_id,
                            uint32_t fb_offset,
                            uint32_t fence_api)
{
   if (!ctx || !fence_id || !fb_res_id)
      return;

   uint8_t type;
   switch (fence_api) {
   case NPT_FENCE_FEEDBACK_API_D3D11:
      type = NPT_FEEDBACK_TYPE_FENCE;
      break;
   case NPT_FENCE_FEEDBACK_API_D3D12:
      type = NPT_FEEDBACK_TYPE_FENCE12;
      break;
   default:
      npt_log("fence feedback: unknown fence_api %u for obj 0x%" PRIx64,
              fence_api, fence_id);
      return;
   }

   struct npt_feedback_entry *entry = npt_feedback_register_locked(
      ctx, fence_id, type, fb_res_id, fb_offset,
      sizeof(struct npt_d3d11_fence_feedback_slot));
   if (!entry)
      return;
   /* Persistent: the entry stays for the fence's lifetime; each
    * Signal-driven poll updates the slot in place.  host_obj is
    * resolved lazily on first Signal. */
   entry->persistent = true;
   npt_feedback_state_unlock(ctx);
}

void
npt_feedback_fence_mark_signal(struct npt_context *ctx, void *host_fence,
                               uint64_t value)
{
   if (!ctx || !host_fence || !ctx->feedback.table)
      return;

   npt_feedback_state_lock(ctx);
   /* Fast path: entry's host_obj already resolved (every Signal
    * after the first hits this).  Both fence types share one host
    * pointer namespace, so scan for either. */
   struct npt_feedback_entry *entry = NULL;
   hash_table_foreach(ctx->feedback.table, e) {
      struct npt_feedback_entry *cand = e->data;
      if (npt_feedback_type_is_fence(cand->type) &&
          cand->host_obj == host_fence) {
         entry = cand;
         break;
      }
   }
   if (!entry) {
      /* First-time resolve: fence registered before any Signal hasn't
       * had host_obj cached.  Walk fence entries with NULL host_obj
       * and bind via the object table.  Linear scan; fence counts are
       * small. */
      hash_table_foreach(ctx->feedback.table, e) {
         struct npt_feedback_entry *cand = e->data;
         if (!npt_feedback_type_is_fence(cand->type) || cand->host_obj)
            continue;
         cand->host_obj = npt_context_lookup_object(
            ctx, NULL, cand->obj_id, NPT_OBJECT_TYPE_IUNKNOWN);
         if (cand->host_obj == host_fence) {
            entry = cand;
            break;
         }
      }
   }
   /* A fence the host has seen rewind owes no further polls: the guest
    * reads such a fence over the wire rather than from the slot. */
   const bool armed = entry && !entry->rewound;
   if (armed) {
      /* Raise the bar the poller has to clear before it may drop this
       * entry.  Signals from different queues reach different ring
       * threads and can be dispatched in either order, so only ever move
       * the target forward. */
      if (value > entry->target_value)
         entry->target_value = value;
      npt_feedback_set_pending_locked(ctx, entry);
   }
   npt_feedback_state_unlock(ctx);

   /* An armed entry is only ever observed by a ring thread's idle-gap
    * poll, and that thread may be parked in an unbounded wait.  Signal
    * usually runs on the ring thread itself, but dispatch can come from
    * elsewhere, so tell every ring a poll is owed rather than relying on
    * that.  After the state unlock: npt_ring_notify_feedback() takes
    * ring->mutex, which the poll path nests the other way round. */
   if (armed)
      npt_context_notify_rings_feedback(ctx);
}

/* Per-type poller for fences.  Caller holds feedback state lock.
 *
 * Publishes the latest GetCompletedValue, then reports whether this
 * entry is done owing polls: true once the completed value has caught up
 * to the highest signalled target, false while an advance is still
 * outstanding.  Dropping the entry from pending loses nothing -- it
 * lives in the table until UNREGISTER and the next Signal re-arms it --
 * and lets pending_count reach zero, which is what a ring thread needs
 * to park rather than poll. */
static bool
npt_feedback_fence_poll_one(struct npt_context *ctx,
                            struct npt_feedback_entry *entry)
{
   /* Entries reach pending only via the Signal hook, which always
    * resolves host_obj first.  An unresolved entry here would be a
    * Signal-hook bug; defensively skip. */
   if (!entry->host_obj || !entry->fb_res_id)
      return false;

   struct npt_resource *res = npt_context_get_resource(ctx, entry->fb_res_id);
   struct npt_d3d11_fence_feedback_slot *slot =
      npt_feedback_slot_ptr(res, entry->fb_offset, sizeof(*slot));
   if (!slot)
      return false;

   /* The substrate poll holds st->mutex across this call so no two
    * threads write the same slot concurrently; the release store
    * publishes the value to the guest's local read.  Plain snapshot
    * store, NOT a monotonic max: D3D12 fences may be legally rewound
    * (Signal(lower)), and for monotonic D3D11 fences the snapshot is
    * a valid lower bound anyway. */
   UINT64 val;
   if (entry->type == NPT_FEEDBACK_TYPE_FENCE12) {
      PFN_ID3D12Fence_GetCompletedValue get12 =
         NPT_COM_VTBL_FUNC(PFN_ID3D12Fence_GetCompletedValue,
                           npt_com_vtable(entry->host_obj),
                           NPT_VTBL_ID3D12Fence_GetCompletedValue);
      val = get12(entry->host_obj);
   } else {
      PFN_ID3D11Fence_GetCompletedValue get11 =
         NPT_COM_VTBL_FUNC(PFN_ID3D11Fence_GetCompletedValue,
                           npt_com_vtable(entry->host_obj),
                           NPT_VTBL_ID3D11Fence_GetCompletedValue);
      val = get11(entry->host_obj);
   }
   atomic_store_explicit(&slot->completed_value, (uint64_t)val,
                         memory_order_release);

   /* D3D12 permits Signal to a lower value, and target_value only ever
    * rises, so a rewound fence could never clear it again: latch the
    * observation and stop owing polls, rather than hold every ring
    * thread of this context in the 100 us feedback cadence for the rest
    * of the session.  Latched on an observed decrease of the value
    * itself, not on the requested one -- a Signal(lower) dispatched
    * ahead of a concurrent Signal(higher) from another queue is
    * ordinary reordering, not a rewind. */
   if ((uint64_t)val < entry->published_high) {
      entry->rewound = true;
      npt_log("fence feedback: obj 0x%" PRIx64 " rewound to %" PRIu64
              " from %" PRIu64 "; guest reads it over the wire from now on",
              entry->obj_id, (uint64_t)val, entry->published_high);
      return true;
   }
   entry->published_high = (uint64_t)val;

   return (uint64_t)val >= entry->target_value;
}
