/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Consumer-side command stream interface for the neptune-protocol.
 *
 * MUST NOT include any neptune-protocol/ header.  The generated
 * protocol headers include this file via npt_protocol_defs.h, so the
 * reverse direction would create a circular include.
 */

#ifndef NPT_CS_H
#define NPT_CS_H

#include "npt_common.h"
#include "npt_com.h"

#define NPT_CS_DECODER_TEMP_POOL_MAX_SIZE (1u * 1024 * 1024 * 1024)

struct npt_resource;
struct npt_context;

/* Full definition comes from the generated
 * npt_protocol_host_dispatch_types.h (included only by dispatch TUs). */
struct npt_dispatch_context;

/* HRESULT shorthand (the protocol uses raw HRESULT). */
#define NPT_S_OK          ((HRESULT)0)
#define NPT_S_FALSE       ((HRESULT)1)
#define NPT_E_NOTIMPL     ((HRESULT)0x80004001)
#define NPT_E_NOINTERFACE ((HRESULT)0x80004002)
#define NPT_E_FAIL        ((HRESULT)0x80004005)
#define NPT_E_INVALIDARG  ((HRESULT)0x80070057)
#define NPT_E_OUTOFMEMORY ((HRESULT)0x8007000E)

#define NPT_SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define NPT_FAILED(hr)    ((HRESULT)(hr) < 0)

struct npt_cs_encoder {
   bool *fatal_error;

   mtx_t mutex;

   struct {
      const struct npt_resource *resource;
      size_t offset;
      size_t size;
      bool busy;
   } stream;

   uint8_t *cur;
   const uint8_t *end;
};

static inline int
npt_cs_encoder_init(struct npt_cs_encoder *enc, bool *fatal_error)
{
   memset(enc, 0, sizeof(*enc));
   enc->fatal_error = fatal_error;

   return mtx_init(&enc->mutex, mtx_plain);
}

static inline void
npt_cs_encoder_fini(struct npt_cs_encoder *enc)
{
   mtx_destroy(&enc->mutex);
}

static inline void
npt_cs_encoder_set_fatal(const struct npt_cs_encoder *enc)
{
   *enc->fatal_error = true;
}

void
npt_cs_encoder_set_stream_locked(struct npt_cs_encoder *enc,
                                 const struct npt_resource *res,
                                 size_t offset,
                                 size_t size);

void
npt_cs_encoder_seek_stream_locked(struct npt_cs_encoder *enc, size_t pos);

static inline void
npt_cs_encoder_set_stream(struct npt_cs_encoder *enc,
                          const struct npt_resource *res,
                          size_t offset,
                          size_t size)
{
   mtx_lock(&enc->mutex);
   npt_cs_encoder_set_stream_locked(enc, res, offset, size);
   mtx_unlock(&enc->mutex);
}

static inline void
npt_cs_encoder_seek_stream(struct npt_cs_encoder *enc, size_t pos)
{
   mtx_lock(&enc->mutex);
   npt_cs_encoder_seek_stream_locked(enc, pos);
   mtx_unlock(&enc->mutex);
}

static inline bool
npt_cs_encoder_check_stream(struct npt_cs_encoder *enc, const struct npt_resource *res)
{
   mtx_lock(&enc->mutex);
   if (enc->stream.resource && enc->stream.resource == res) {
      if (enc->stream.busy) {
         mtx_unlock(&enc->mutex);
         return false;
      }
      npt_cs_encoder_set_stream_locked(enc, NULL, 0, 0);
   }
   mtx_unlock(&enc->mutex);

   return true;
}

static inline bool
npt_cs_encoder_acquire(struct npt_cs_encoder *enc)
{
   mtx_lock(&enc->mutex);
   if (unlikely(!enc->stream.resource)) {
      /* Replying with no destination is a programming error.  Mark
       * fatal so the context tears down — silently dropping the reply
       * would spin the guest in get_command_reply. */
      npt_log("encoder_acquire: no stream set -- missing SET_REPLY_STREAM");
      npt_cs_encoder_set_fatal(enc);
      mtx_unlock(&enc->mutex);
      return false;
   }
   enc->stream.busy = true;
   mtx_unlock(&enc->mutex);
   return true;
}

static inline void
npt_cs_encoder_release(struct npt_cs_encoder *enc)
{
   mtx_lock(&enc->mutex);
   assert(enc->stream.resource);
   enc->stream.busy = false;
   mtx_unlock(&enc->mutex);
}

static inline void
npt_cs_encoder_write(struct npt_cs_encoder *enc,
                     size_t size,
                     const void *val,
                     size_t val_size)
{
   assert(val_size <= size);

   if (unlikely(size > (size_t)(enc->end - enc->cur))) {
      npt_log("failed to write the reply stream");
      npt_cs_encoder_set_fatal(enc);
      return;
   }

   if (enc->cur != val)
      memcpy(enc->cur, val, val_size);
   enc->cur += size;
}

struct npt_cs_decoder_temp_pool {
   uint8_t **buffers;
   uint32_t buffer_count;
   uint32_t buffer_max;
   size_t total_size;

   uint8_t *reset_to;

   uint8_t *cur;
   const uint8_t *end;
};

/* Outer stream + temp pool state saved across EXECUTE_COMMAND_STREAM.
 * One level of nesting only. */
struct npt_cs_decoder_saved_state {
   const uint8_t *cur;
   const uint8_t *end;

   uint32_t pool_buffer_count;
   uint8_t *pool_reset_to;
};

/* Per-decoder object-handle lookup cache (see npt_context_lookup_object).
 * Every ring thread owns one decoder, so this is private to the thread
 * and a hit touches no shared cache line -- the table's mutex would cost
 * every ring thread an exclusive line acquisition per handle argument.
 * Direct-mapped; flushed whole when the context's object generation moves
 * (any unregister / type change). */
#define NPT_CS_LOOKUP_CACHE_SIZE 256u
struct npt_cs_lookup_entry {
   uint64_t id;
   void *host_ptr;
   uint32_t type;
};

struct npt_cs_decoder {
   bool *fatal_error;
   struct npt_cs_decoder_temp_pool temp_pool;

   uint64_t lookup_gen;
   struct npt_cs_lookup_entry lookup_cache[NPT_CS_LOOKUP_CACHE_SIZE];

   struct npt_cs_decoder_saved_state saved_state;
   bool saved_state_valid;

   /* Serialises destroy-vs-read on the bound SHM blob resource. */
   mtx_t resource_mutex;
   const struct npt_resource *resource;

   const uint8_t *cur;
   const uint8_t *end;
};

int
npt_cs_decoder_init(struct npt_cs_decoder *dec, bool *fatal_error);

void
npt_cs_decoder_fini(struct npt_cs_decoder *dec);

void
npt_cs_decoder_reset(struct npt_cs_decoder *dec);

static inline void
npt_cs_decoder_set_fatal(const struct npt_cs_decoder *dec)
{
   *((struct npt_cs_decoder *)dec)->fatal_error = true;
}

static inline bool
npt_cs_decoder_get_fatal(const struct npt_cs_decoder *dec)
{
   return *dec->fatal_error;
}

static inline void
npt_cs_decoder_set_buffer_stream(struct npt_cs_decoder *dec,
                                 const void *data,
                                 size_t size)
{
   dec->cur = data;
   dec->end = dec->cur + size;
}

static inline bool
npt_cs_decoder_check_stream(struct npt_cs_decoder *dec, const struct npt_resource *res)
{
   mtx_lock(&dec->resource_mutex);
   const bool ok = dec->resource != res;
   mtx_unlock(&dec->resource_mutex);
   return ok;
}

static inline bool
npt_cs_decoder_has_command(const struct npt_cs_decoder *dec)
{
   return dec->cur < dec->end;
}

static inline bool
npt_cs_decoder_has_saved_state(const struct npt_cs_decoder *dec)
{
   return dec->saved_state_valid;
}

void
npt_cs_decoder_save_state(struct npt_cs_decoder *dec);

void
npt_cs_decoder_restore_state(struct npt_cs_decoder *dec);

static inline void
npt_cs_decoder_read(struct npt_cs_decoder *dec, size_t size, void *val, size_t val_size)
{
   assert(val_size <= size);

   if (unlikely(size > (size_t)(dec->end - dec->cur))) {
      npt_log("failed to read %zu bytes (remaining=%zu)", size, (size_t)(dec->end - dec->cur));
      npt_cs_decoder_set_fatal(dec);
      memset(val, 0, val_size);
      return;
   }

   if (dec->cur != val)
      memcpy(val, dec->cur, val_size);
   dec->cur += size;
}

static inline void
npt_cs_decoder_peek(const struct npt_cs_decoder *dec,
                    size_t size,
                    void *val,
                    size_t val_size)
{
   assert(val_size <= size);

   if (unlikely(size > (size_t)(dec->end - dec->cur))) {
      npt_log("failed to peek %zu bytes", size);
      npt_cs_decoder_set_fatal(dec);
      memset(val, 0, val_size);
      return;
   }

   if (dec->cur != val)
      memcpy(val, dec->cur, val_size);
}

bool
npt_cs_decoder_alloc_temp_internal(struct npt_cs_decoder *dec, size_t size);

static inline void *
npt_cs_decoder_alloc_temp(struct npt_cs_decoder *dec, size_t size)
{
   struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;

   if (unlikely(size > (size_t)(pool->end - pool->cur))) {
      if (!npt_cs_decoder_alloc_temp_internal(dec, size)) {
         npt_log("failed to suballocate %zu bytes from the temp pool", size);
         npt_cs_decoder_set_fatal(dec);
         return NULL;
      }
   }

   /* align to 64-bit */
   size = align64(size, 8);
   assert(size <= (size_t)(pool->end - pool->cur));

   void *ptr = pool->cur;
   pool->cur += size;
   return ptr;
}

static inline void *
npt_cs_decoder_alloc_temp_array(struct npt_cs_decoder *dec,
                                size_t element_size,
                                size_t count)
{
   size_t total;
   if (unlikely(__builtin_mul_overflow(element_size, count, &total))) {
      npt_log("overflow in array allocation of %zu * %zu bytes",
              element_size, count);
      npt_cs_decoder_set_fatal(dec);
      return NULL;
   }
   return npt_cs_decoder_alloc_temp(dec, total);
}

static inline void
npt_cs_decoder_reset_temp_pool(struct npt_cs_decoder *dec)
{
   struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;
   pool->cur = pool->reset_to;
}

/* Zero-copy access for large payloads.  Returns NULL when fewer than
 * `size` bytes remain; caller advances dec->cur after consuming. */
static inline void *
npt_cs_decoder_get_blob_storage(struct npt_cs_decoder *dec, size_t size)
{
   return unlikely(size > (size_t)(dec->end - dec->cur)) ? NULL
                                                         : (void *)dec->cur;
}

static inline void *
npt_cs_encoder_get_blob_storage(struct npt_cs_encoder *enc, size_t offset,
                                size_t size)
{
   return unlikely(offset + size > (size_t)(enc->end - enc->cur))
             ? NULL
             : (void *)(enc->cur + offset);
}

/* Wire object_id is the host-side COM pointer cast to uint64_t.
 * Decode via npt_object_from_id, then validate / translate through
 * npt_cs_handle_lookup. */

static inline npt_object_id
npt_object_get_id(const void *handle)
{
   return (npt_object_id)(uintptr_t)handle;
}

static inline void *
npt_object_from_id(npt_object_id id)
{
   return (void *)(uintptr_t)id;
}

/* Implemented in npt_context.c (forward-declared here to avoid a
 * cycle through npt_context.h -> generated dispatch types -> this
 * header). */
void *
npt_cs_handle_lookup(struct npt_dispatch_context *ctx,
                     npt_object_id id,
                     npt_object_type type);

/* Safe with id == 0 or obj == NULL. */
void
npt_cs_handle_register_guest_id(struct npt_dispatch_context *ctx,
                                uint64_t guest_id,
                                void *obj,
                                npt_object_type type);

/* Win32 handles are opaque on the wire: identity pass-through.  Identity
 * preserves the KMT-vs-NT high-bit dispatch host D3D libraries use to
 * route OpenSharedResource. */
static inline npt_object_id
npt_win32_handle_get_id(const void *handle)
{
   return (npt_object_id)(uintptr_t)handle;
}

static inline void *
npt_win32_handle_from_id(npt_object_id id)
{
   return (void *)(uintptr_t)id;
}

/* Non-event Win32 HANDLEs (HWND, shared-resource HANDLEs) round-trip
 * unchanged. */
static inline void *
npt_win32_handle_replace(struct npt_dispatch_context *ctx,
                         npt_object_id id)
{
   (void)ctx;
   return (void *)(uintptr_t)id;
}

/* Host-side event proxy for a guest event token, registered by
 * REGISTER_EVENT / ARM_EVENT_FENCE, or NULL when the token has none. */
void *npt_event_replace_by_token(struct npt_dispatch_context *dispatch,
                                  npt_object_id id);

/* Event HANDLEs resolve to their host proxy.  An unregistered token maps
 * to NULL rather than the identity fallback, so a guest that skipped
 * REGISTER_EVENT cannot hand a raw token to a backend that dereferences
 * the handle. */
static inline void *
npt_event_handle_replace(struct npt_dispatch_context *ctx,
                         npt_object_id id)
{
   return npt_event_replace_by_token(ctx, id);
}

#endif /* NPT_CS_H */
