/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Out-of-line encoder/decoder helpers.  The hot read/write paths are
 * inlined in npt_cs.h.
 */

#include "npt_cs.h"

#include "npt_context.h"

void
npt_cs_encoder_set_stream_locked(struct npt_cs_encoder *enc,
                                 const struct npt_resource *res,
                                 size_t offset,
                                 size_t size)
{
   if (!res) {
      memset(&enc->stream, 0, sizeof(enc->stream));
      enc->cur = NULL;
      enc->end = NULL;
      return;
   }

   if (unlikely(size > res->size || offset > res->size - size)) {
      npt_log(
         "failed to set the reply stream: offset(%zu) + size(%zu) exceeds res size(%zu)",
         offset, size, res->size);
      npt_cs_encoder_set_fatal(enc);
      return;
   }

   enc->stream.resource = res;
   enc->stream.offset = offset;
   enc->stream.size = size;

   enc->end = res->u.data + res->size;

   npt_cs_encoder_seek_stream_locked(enc, 0);
}

void
npt_cs_encoder_seek_stream_locked(struct npt_cs_encoder *enc, size_t pos)
{
   if (unlikely(!enc->stream.resource || pos > enc->stream.size)) {
      npt_log("failed to seek the reply stream to %zu", pos);
      npt_cs_encoder_set_fatal(enc);
      return;
   }

   enc->cur = enc->stream.resource->u.data + enc->stream.offset + pos;
}

int
npt_cs_decoder_init(struct npt_cs_decoder *dec, bool *fatal_error)
{
   memset(dec, 0, sizeof(*dec));
   dec->fatal_error = fatal_error;
   return mtx_init(&dec->resource_mutex, mtx_plain);
}

void
npt_cs_decoder_fini(struct npt_cs_decoder *dec)
{
   struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;
   for (uint32_t i = 0; i < pool->buffer_count; i++)
      free(pool->buffers[i]);
   if (pool->buffers)
      free(pool->buffers);

   mtx_destroy(&dec->resource_mutex);
}

static void
npt_cs_decoder_sanity_check(const struct npt_cs_decoder *dec)
{
   const struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;
   assert(pool->buffer_count <= pool->buffer_max);
   if (pool->buffer_count) {
      assert(pool->buffers[pool->buffer_count - 1] <= pool->reset_to);
      assert(pool->reset_to <= pool->cur);
      assert(pool->cur <= pool->end);
   }

   assert(dec->cur <= dec->end);
}

static void
npt_cs_decoder_gc_temp_pool(struct npt_cs_decoder *dec)
{
   struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;
   if (!pool->buffer_count)
      return;

   /* keep only the last buffer */
   if (pool->buffer_count > 1) {
      for (uint32_t i = 0; i < pool->buffer_count - 1; i++)
         free(pool->buffers[i]);

      pool->buffers[0] = pool->buffers[pool->buffer_count - 1];
      pool->buffer_count = 1;
   }

   pool->reset_to = pool->buffers[0];
   pool->cur = pool->buffers[0];

   pool->total_size = pool->end - pool->cur;

   npt_cs_decoder_sanity_check(dec);
}

void
npt_cs_decoder_reset(struct npt_cs_decoder *dec)
{
   /* dec->fatal_error is sticky. */

   npt_cs_decoder_gc_temp_pool(dec);

   /* Defensive: an unmatched save would leak state into the next reset. */
   dec->saved_state_valid = false;
   dec->handle_miss = false;

   /* No lock needed: reset() runs only between submissions on the
    * owning ring, with no concurrent reader. */
   dec->resource = NULL;
   dec->cur = NULL;
   dec->end = NULL;
}

void
npt_cs_decoder_save_state(struct npt_cs_decoder *dec)
{
   assert(!dec->saved_state_valid);
   dec->saved_state_valid = true;

   struct npt_cs_decoder_saved_state *saved = &dec->saved_state;
   saved->cur = dec->cur;
   saved->end = dec->end;

   /* Advance reset_to so the nested command's per-command
    * reset_temp_pool can't free outer-frame allocations. */
   struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;
   saved->pool_buffer_count = pool->buffer_count;
   saved->pool_reset_to = pool->reset_to;
   pool->reset_to = pool->cur;
}

void
npt_cs_decoder_restore_state(struct npt_cs_decoder *dec)
{
   assert(dec->saved_state_valid);
   dec->saved_state_valid = false;

   /* Forget the nested SHM-blob resource binding so a later destroy
    * doesn't match this stale pointer. */
   dec->resource = NULL;

   const struct npt_cs_decoder_saved_state *saved = &dec->saved_state;
   dec->cur = saved->cur;
   dec->end = saved->end;

   /* If a new temp-pool buffer was appended during nested execution
    * the saved reset_to points into the wrong buffer.  Leave reset_to
    * alone; the new buffer's own reset_to protects outer data. */
   struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;
   if (pool->buffer_count == saved->pool_buffer_count)
      pool->reset_to = saved->pool_reset_to;
}

static uint32_t
next_array_size(uint32_t cur_size, uint32_t min_size)
{
   const uint32_t next_size = cur_size ? cur_size * 2 : min_size;
   return next_size > cur_size ? next_size : 0;
}

static size_t
next_buffer_size(size_t cur_size, size_t min_size, size_t need)
{
   size_t next_size = cur_size ? cur_size * 2 : min_size;
   while (next_size < need) {
      next_size *= 2;
      if (!next_size)
         return 0;
   }
   return next_size;
}

static bool
npt_cs_decoder_grow_temp_pool(struct npt_cs_decoder *dec)
{
   struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;
   const uint32_t buf_max = next_array_size(pool->buffer_max, 4);
   if (!buf_max)
      return false;

   uint8_t **bufs = realloc(pool->buffers, sizeof(*pool->buffers) * buf_max);
   if (!bufs)
      return false;

   pool->buffers = bufs;
   pool->buffer_max = buf_max;

   return true;
}

bool
npt_cs_decoder_alloc_temp_internal(struct npt_cs_decoder *dec, size_t size)
{
   struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;

   if (pool->buffer_count >= pool->buffer_max) {
      if (!npt_cs_decoder_grow_temp_pool(dec))
         return false;
      assert(pool->buffer_count < pool->buffer_max);
   }

   const size_t cur_buf_size =
      pool->buffer_count ? pool->end - pool->buffers[pool->buffer_count - 1] : 0;
   const size_t buf_size = next_buffer_size(cur_buf_size, 4096, size);
   if (!buf_size)
      return false;

   if (buf_size > NPT_CS_DECODER_TEMP_POOL_MAX_SIZE - pool->total_size)
      return false;

   uint8_t *buf = malloc(buf_size);
   if (!buf)
      return false;

   pool->total_size += buf_size;
   pool->buffers[pool->buffer_count++] = buf;
   pool->reset_to = buf;
   pool->cur = buf;
   pool->end = buf + buf_size;

   npt_cs_decoder_sanity_check(dec);

   return true;
}
