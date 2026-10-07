/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * CREATE_HEAP_FROM_SHMEM: import a guest SHM blob window as an
 * ID3D12Heap, so the guest's persistent Map pointer and the GPU alias
 * the same pages.
 *
 * While an import is alive, munmapping the blob is undefined -- the
 * host graphics driver still owns those pages.  Each import pins the
 * backing npt_resource, so a destroy arriving first only marks the
 * resource a zombie and the last import released completes the free.
 */

#include "npt_heap12.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/udmabuf.h>
#endif

#include "npt_com.h"
#include "npt_context.h"
#include "npt_cs.h"
#include "npt_library.h"
#include "npt_renderer.h"
#include "npt_transport_defs.h"

struct npt_heap_import {
   uint64_t heap_id;              /* guest id of the ID3D12Heap */
   struct npt_resource *res;      /* backing SHM resource */
};

#ifdef __linux__
/* Wrap [offset, offset + size) of the resource's memfd in a udmabuf.
 * Requires the memfd to carry F_SEAL_SHRINK.  Returns the dmabuf fd
 * or -1. */
static int
npt_heap12_wrap_udmabuf(const struct npt_resource *res,
                        uint32_t offset, uint64_t size)
{
   struct udmabuf_create uc;
   int dev_fd, buf_fd;

   dev_fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
   if (dev_fd < 0) {
      npt_log("create_heap_from_shmem: open(/dev/udmabuf) failed "
              "(errno %d); persistent-map heaps unavailable, guest "
              "degrades to sync map", errno);
      return -1;
   }

   memset(&uc, 0, sizeof(uc));
   uc.memfd = (uint32_t)res->u.fd;
   uc.flags = UDMABUF_FLAGS_CLOEXEC;
   uc.offset = offset;
   uc.size = size;

   buf_fd = ioctl(dev_fd, UDMABUF_CREATE, &uc);
   if (buf_fd < 0)
      npt_log("create_heap_from_shmem: UDMABUF_CREATE(res %u memfd %d "
              "off %u size %" PRIu64 ") failed (errno %d); guest "
              "degrades to sync map",
              res->res_id, res->u.fd, offset, size, errno);
   close(dev_fd);
   return buf_fd;
}
#endif /* __linux__ */

#if defined(__linux__)
static bool
npt_heap12_import_available(const struct npt_d3d_library *lib)
{
   return lib && lib->pfn_vkd3d_open_existing_heap_from_dmabuf;
}

/* The window travels as a dmabuf rather than as a host pointer because
 * amdgpu's userptr path rejects VK_EXT_external_memory_host imports of
 * memfd-backed pages. */
static HRESULT
npt_heap12_import(const struct npt_d3d_library *lib, void *device,
                  const struct npt_resource *res,
                  const struct npt_cmd_create_heap_from_shmem *cmd,
                  void **out_heap)
{
   const int dmabuf_fd =
      npt_heap12_wrap_udmabuf(res, cmd->shmem_offset, cmd->size);
   if (dmabuf_fd < 0)
      return NPT_E_FAIL;

   const HRESULT hr = lib->pfn_vkd3d_open_existing_heap_from_dmabuf(
      device, dmabuf_fd, cmd->size, &NPT_IID_ID3D12Heap, out_heap);
   close(dmabuf_fd);
   return hr;
}
#elif defined(__APPLE__)
static bool
npt_heap12_import_available(const struct npt_d3d_library *lib)
{
   return lib && lib->pfn_darwin_open_existing_heap_from_fd;
}

/* No udmabuf to carve the window with, so the whole blob's fd travels
 * with an explicit offset. */
static HRESULT
npt_heap12_import(const struct npt_d3d_library *lib, void *device,
                  const struct npt_resource *res,
                  const struct npt_cmd_create_heap_from_shmem *cmd,
                  void **out_heap)
{
   return lib->pfn_darwin_open_existing_heap_from_fd(
      device, res->u.fd, cmd->shmem_offset, cmd->size,
      cmd->heap_type, cmd->heap_flags, &NPT_IID_ID3D12Heap, out_heap);
}
#else
static bool
npt_heap12_import_available(UNUSED const struct npt_d3d_library *lib)
{
   return false;
}

static HRESULT
npt_heap12_import(UNUSED const struct npt_d3d_library *lib,
                  UNUSED void *device,
                  UNUSED const struct npt_resource *res,
                  UNUSED const struct npt_cmd_create_heap_from_shmem *cmd,
                  UNUSED void **out_heap)
{
   return NPT_E_NOTIMPL;
}
#endif

/* Obtain the host ID3D12Heap for the window [shmem_offset, +size) of the
 * pinned SHM resource.  On success *out_heap is an AddRef'd heap; every
 * failure is one the guest recovers from by falling back to sync map. */
static HRESULT
npt_heap12_open_host_heap(void *device, struct npt_resource *res,
                          const struct npt_cmd_create_heap_from_shmem *cmd,
                          void **out_heap)
{
   const struct npt_d3d_library *lib = npt_renderer_get_library();

   if (!npt_heap12_import_available(lib)) {
      npt_log("create_heap_from_shmem: backend has no heap-import entry "
              "point; guest degrades to sync map");
      return NPT_E_NOTIMPL;
   }

   if (res->u.fd < 0) {
      npt_log("create_heap_from_shmem: res %u has no backing fd; guest "
              "degrades to sync map", cmd->shmem_res_id);
      return NPT_E_INVALIDARG;
   }

   const HRESULT hr = npt_heap12_import(lib, device, res, cmd, out_heap);
   if (NPT_FAILED(hr) || !*out_heap)
      npt_log("create_heap_from_shmem: import of res=%u off=%u size=%" PRIu64
              " type=%u flags=0x%x failed 0x%08x",
              cmd->shmem_res_id, cmd->shmem_offset, cmd->size,
              cmd->heap_type, cmd->heap_flags, (unsigned)hr);
   return hr;
}

static HRESULT
npt_heap12_create_from_shmem(struct npt_context *ctx,
                             struct npt_cs_decoder *dec,
                             const struct npt_cmd_create_heap_from_shmem *cmd)
{
   const uint64_t device_id = cmd->header.object_id;

   if (!cmd->mint_heap_id || !cmd->size) {
      npt_log("create_heap_from_shmem: invalid args (mint_heap_id=0x%016"
              PRIx64 ", size=%" PRIu64 ")", cmd->mint_heap_id, cmd->size);
      return NPT_E_INVALIDARG;
   }

   /* Pinned for the whole import: everything below runs outside
    * resource_mutex and a concurrent DESTROY_RESOURCE would otherwise
    * free the mapping under it.  On success the pin becomes the
    * import's; every failure path drops it. */
   struct npt_resource *res =
      npt_context_pin_shm_resource(ctx, cmd->shmem_res_id);
   if (!res) {
      npt_log("create_heap_from_shmem: res %u is not a mapped SHM resource",
              cmd->shmem_res_id);
      return NPT_E_INVALIDARG;
   }

   HRESULT hr;

   const long page_size = sysconf(_SC_PAGESIZE);
   if (page_size > 0 &&
       ((cmd->shmem_offset & ((uint64_t)page_size - 1)) ||
        (cmd->size & ((uint64_t)page_size - 1)))) {
      npt_log("create_heap_from_shmem: window (off %u, size %" PRIu64
              ") not page-aligned", cmd->shmem_offset, cmd->size);
      hr = NPT_E_INVALIDARG;
      goto err_unpin;
   }

   if ((uint64_t)cmd->shmem_offset + cmd->size > (uint64_t)res->size) {
      npt_log("create_heap_from_shmem: window [%u, +%" PRIu64
              ") exceeds res %u size %zu",
              cmd->shmem_offset, cmd->size, cmd->shmem_res_id, res->size);
      hr = NPT_E_INVALIDARG;
      goto err_unpin;
   }

   void *device = npt_context_lookup_object(ctx, dec, device_id,
                                            NPT_OBJECT_TYPE_IUNKNOWN);
   if (!device) {
      npt_log("create_heap_from_shmem: device id 0x%016" PRIx64
              " not registered", device_id);
      hr = NPT_E_INVALIDARG;
      goto err_unpin;
   }

   void *heap = NULL;
   hr = npt_heap12_open_host_heap(device, res, cmd, &heap);
   if (NPT_FAILED(hr) || !heap) {
      if (NPT_SUCCEEDED(hr))
         hr = NPT_E_FAIL;
      goto err_unpin;
   }

   struct npt_heap_import *imp = calloc(1, sizeof(*imp));
   if (!imp) {
      npt_com_release(heap);
      hr = NPT_E_OUTOFMEMORY;
      goto err_unpin;
   }
   imp->heap_id = cmd->mint_heap_id;
   imp->res = res;

   mtx_lock(&ctx->heap_import_mutex);
   if (_mesa_hash_table_search(ctx->heap_import_table, &imp->heap_id)) {
      mtx_unlock(&ctx->heap_import_mutex);
      npt_log("create_heap_from_shmem: duplicate heap id 0x%016" PRIx64,
              cmd->mint_heap_id);
      free(imp);
      npt_com_release(heap);
      hr = NPT_E_INVALIDARG;
      goto err_unpin;
   }
   _mesa_hash_table_insert(ctx->heap_import_table, &imp->heap_id, imp);
   mtx_unlock(&ctx->heap_import_mutex);

   npt_context_register_object(ctx, cmd->mint_heap_id, heap,
                               NPT_OBJECT_TYPE_ID3D12HEAP);

   npt_log("create_heap_from_shmem: heap 0x%016" PRIx64 " imports res %u "
           "(off=%u size=%" PRIu64 " app_type=%u app_flags=0x%x)",
           cmd->mint_heap_id, cmd->shmem_res_id, cmd->shmem_offset,
           cmd->size, cmd->heap_type, cmd->heap_flags);

   return NPT_S_OK;

err_unpin:
   npt_context_unpin_resource(ctx, res);
   return hr;
}

void
npt_dispatch_create_heap_from_shmem(struct npt_context *ctx,
                                    struct npt_cs_decoder *dec,
                                    struct npt_cs_encoder *enc,
                                    const struct npt_command_header *header)
{
   struct npt_cmd_create_heap_from_shmem cmd;
   cmd.header = *header;
   npt_cs_decoder_read(dec, sizeof(cmd) - sizeof(cmd.header),
                       &cmd.mint_heap_id, sizeof(cmd) - sizeof(cmd.header));
   if (npt_cs_decoder_get_fatal(dec))
      return;

   const HRESULT hr = npt_heap12_create_from_shmem(ctx, dec, &cmd);

   if (header->cmd_flags & NPT_CMD_FLAG_REPLY) {
      struct npt_cmd_create_heap_from_shmem_reply reply;
      memset(&reply, 0, sizeof(reply));
      reply.header.cmd_type = header->cmd_type;
      reply.header.cmd_return = (uint32_t)hr;
      if (npt_cs_encoder_acquire(enc)) {
         npt_cs_encoder_write(enc, sizeof(reply), &reply, sizeof(reply));
         npt_cs_encoder_release(enc);
      }
   }
}

static void
npt_heap12_drop_import(struct npt_context *ctx, struct npt_heap_import *imp)
{
   npt_context_unpin_resource(ctx, imp->res);
   free(imp);
}

void
npt_heap12_on_release_object(struct npt_context *ctx, uint64_t guest_id)
{
   if (!guest_id || !ctx->heap_import_table)
      return;

   mtx_lock(&ctx->heap_import_mutex);
   struct hash_entry *entry =
      _mesa_hash_table_search(ctx->heap_import_table, &guest_id);
   struct npt_heap_import *imp = entry ? entry->data : NULL;
   if (entry)
      _mesa_hash_table_remove(ctx->heap_import_table, entry);
   mtx_unlock(&ctx->heap_import_mutex);

   if (!imp)
      return;

   npt_heap12_drop_import(ctx, imp);
}

void
npt_heap12_context_fini(struct npt_context *ctx)
{
   if (!ctx->heap_import_table)
      return;

   hash_table_foreach(ctx->heap_import_table, entry) {
      struct npt_heap_import *imp = entry->data;
      npt_log("heap12: context teardown with heap 0x%016" PRIx64
              " still importing res %u", imp->heap_id, imp->res->res_id);
      npt_heap12_drop_import(ctx, imp);
   }
   _mesa_hash_table_clear(ctx->heap_import_table, NULL);
}
