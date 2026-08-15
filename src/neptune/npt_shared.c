/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11 shared / presentable textures over virtio-gpu blob resources.
 * See npt_shared.h for the model.  The COM flow (GetSharedHandle /
 * OpenSharedResource) is platform-neutral; only the descriptor behind
 * the HANDLE differs: dxvk's DxvkSharedTextureDescriptor (dmabuf) on
 * Linux, the darwin backend's shared-texture descriptor (shm fd) on macOS.
 */

#include "npt_shared.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef __APPLE__
#include <dxvk_shared_resource.h>
#endif

#include "c11/threads.h"

#include "virgl_hw.h"

#include "npt_context.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

/* D3D11_BIND_SHADER_RESOURCE (d3d11.h); consumers sample the texture. */
#define NPT_D3D11_BIND_SHADER_RESOURCE 0x8u

/* The exporting backend's own description of the texture format.  The
 * Darwin backend records the DXGI format directly; the dxvk backend
 * carries it in the shared-resource metadata. */
#ifdef __APPLE__
#define NPT_SHARED_EXPORT_FORMAT(d) ((d)->dxgi_format)
#else
#define NPT_SHARED_EXPORT_FORMAT(d) ((d)->meta.Format)
#endif

/* DXGI_FORMAT -> enum virgl_formats, for the 8-bit RGBA/BGRA families
 * that can back a presentable surface.  0 means "unmapped", which leaves
 * the importer's own format in charge. */
static uint32_t
npt_shared_dxgi_to_virgl_format(uint32_t dxgi_format)
{
   switch (dxgi_format) {
   case 87: /* DXGI_FORMAT_B8G8R8A8_UNORM */
      return VIRGL_FORMAT_B8G8R8A8_UNORM;
   case 88: /* DXGI_FORMAT_B8G8R8X8_UNORM */
      return VIRGL_FORMAT_B8G8R8X8_UNORM;
   case 28: /* DXGI_FORMAT_R8G8B8A8_UNORM */
      return VIRGL_FORMAT_R8G8B8A8_UNORM;
   default:
      return 0;
   }
}

#ifdef __APPLE__
/* The POD both darwin backends hand through GetSharedHandle /
 * OpenSharedResource, defined here so the renderer neither links nor
 * includes a backend header.  d3dmetal's dmn_shared_texture_handle and
 * dxmt's dxmt_shared_texture_handle share this exact ABI: same 'DMTX'
 * magic, version, and field layout. */
#define NPT_DARWIN_SHARED_TEXTURE_MAGIC   0x58544D44u /* 'DMTX' */
/* Every peer that builds or reads the POD declares the same version, and
 * an import refuses a handle whose version differs rather than read a
 * layout it does not have. */
#define NPT_DARWIN_SHARED_HANDLE_VERSION  2u
struct npt_darwin_shared_texture {
   uint32_t magic;
   uint32_t version;
   int32_t  fd;            /* process-local; sent via SCM_RIGHTS, then patched */
   uint32_t width, height;
   uint32_t dxgi_format;
   uint32_t mip_levels, array_size, sample_count;
   uint32_t bind_flags, misc_flags, cpu_access;
   uint64_t stride;        /* bytesPerRow */
   uint64_t size;          /* logical stride*height */
   uint64_t offset;        /* byte offset of the surface within fd (a
                            * surface placed in a shared heap is a window
                            * into the heap's one object); 0 otherwise */
};
#endif

/* OPEN_RES arrives on the consumer's ring thread; the resource fd
 * arrives on the dispatch thread via the proxy's attach-forwarding
 * (triggered by the guest KMD's CTX_ATTACH_RESOURCE, a virtio ctrl
 * command that is not ordered against ring commands).  Bounded poll
 * bridges the race; the budget stays well under dxgkrnl's ~2 s TDR
 * so a missing attach fails the open instead of wedging the ring. */
#define NPT_SHARED_ATTACH_WAIT_MS   1000
#define NPT_SHARED_ATTACH_POLL_MS   2

HRESULT
npt_shared_export_blob(struct npt_context *ctx, uint64_t texture_id,
                       uint64_t blob_id, uint32_t data_res_id,
                       uint32_t data_off)
{
   if (!texture_id || !blob_id)
      return NPT_E_INVALIDARG;

   void *texture = npt_context_lookup_object(ctx, NULL, texture_id,
                                             NPT_OBJECT_TYPE_IUNKNOWN);
   if (!texture) {
      npt_log("shared: export: texture id 0x%016" PRIx64 " not found",
              texture_id);
      return NPT_E_INVALIDARG;
   }

   /* Export the texture's shared descriptor: D3D12 resources through
    * the device's CreateSharedHandle, D3D11 textures through
    * IDXGIResource::GetSharedHandle.
    *
    * The returned HANDLE is a pointer to a descriptor the exporting
    * object owns and keeps valid until it is destroyed, so nothing here
    * frees it; only copies -- a dup of desc->fd and the scalar fields --
    * leave this frame. */
   HANDLE handle = 0;
   HRESULT hr;
   void *res12 = NULL;
   if (NPT_SUCCEEDED(npt_com_query_interface(texture,
                                             &NPT_IID_ID3D12Resource,
                                             &res12)) && res12) {
      void *dev12 = NULL;
      PFN_ID3D12DeviceChild_GetDevice get_dev12 =
         NPT_COM_VTBL_FUNC(PFN_ID3D12DeviceChild_GetDevice,
                           npt_com_vtable(res12),
                           NPT_VTBL_ID3D12DeviceChild_GetDevice);
      hr = get_dev12(res12, &NPT_IID_ID3D12Device, &dev12);
      if (NPT_FAILED(hr) || !dev12) {
         npt_com_release(res12);
         npt_log("shared: export: blob_id %" PRIu64 " GetDevice failed",
                 blob_id);
         return NPT_E_FAIL;
      }

      PFN_ID3D12Device_CreateSharedHandle create_shared =
         NPT_COM_VTBL_FUNC(PFN_ID3D12Device_CreateSharedHandle,
                           npt_com_vtable(dev12),
                           NPT_VTBL_ID3D12Device_CreateSharedHandle);
      hr = create_shared(dev12, res12, NULL, 0x10000000u, NULL, &handle);
      npt_com_release(dev12);
      npt_com_release(res12);
   } else {
      void *dxgi_res = NULL;
      if (NPT_FAILED(npt_com_query_interface(texture, &NPT_IID_IDXGIResource,
                                             &dxgi_res)) || !dxgi_res) {
         npt_log("shared: export: blob_id %" PRIu64 " has no IDXGIResource",
                 blob_id);
         return NPT_E_FAIL;
      }

      PFN_IDXGIResource_GetSharedHandle get_shared =
         NPT_COM_VTBL_FUNC(PFN_IDXGIResource_GetSharedHandle,
                           npt_com_vtable(dxgi_res),
                           NPT_VTBL_IDXGIResource_GetSharedHandle);
      hr = get_shared(dxgi_res, &handle);
      npt_com_release(dxgi_res);
   }

   if (NPT_FAILED(hr) || !handle) {
      npt_log("shared: export: blob_id %" PRIu64
              " shared-handle export failed (hr=0x%x)", blob_id, hr);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   struct npt_blob_export_info info;
   memset(&info, 0, sizeof(info));

#ifdef __APPLE__
   const struct npt_darwin_shared_texture *desc =
      (const struct npt_darwin_shared_texture *)(uintptr_t)handle;
   if (desc->magic != NPT_DARWIN_SHARED_TEXTURE_MAGIC ||
       desc->version != NPT_DARWIN_SHARED_HANDLE_VERSION || desc->fd < 0) {
      npt_log("shared: export: blob_id %" PRIu64 " bad descriptor", blob_id);
      return NPT_E_FAIL;
   }

   if (!(desc->bind_flags & NPT_D3D11_BIND_SHADER_RESOURCE))
      npt_log("shared: export: blob_id %" PRIu64 " texture lacks "
              "SHADER_RESOURCE bind (0x%x); consumers cannot sample it",
              blob_id, desc->bind_flags);

   /* Backend shared textures are always one linear plane of shared
    * memory; modifier/texture_layout have no meaning here. */
   info.allocation_size = desc->size;
   info.plane_count = 1;
   info.planes[0].offset = desc->offset;
   info.planes[0].pitch = desc->stride;

   const int export_fd = desc->fd;
   /* The blob has to span the window, not just the surface: a placed
    * texture starts `offset` into the heap's object. */
   const uint64_t export_size = desc->offset + desc->size;
#else
   const struct DxvkSharedTextureDescriptor *desc =
      (const struct DxvkSharedTextureDescriptor *)(uintptr_t)handle;
   if (desc->magic != DXVK_SHARED_DESCRIPTOR_TEXTURE ||
       desc->structSize != sizeof(*desc) || desc->fd < 0 ||
       desc->planeCount < 1 ||
       desc->planeCount > NPT_BLOB_EXPORT_MAX_PLANES) {
      npt_log("shared: export: blob_id %" PRIu64 " bad descriptor", blob_id);
      return NPT_E_FAIL;
   }

   if (!(desc->meta.BindFlags & NPT_D3D11_BIND_SHADER_RESOURCE))
      npt_log("shared: export: blob_id %" PRIu64 " texture lacks "
              "SHADER_RESOURCE bind (0x%x); consumers cannot sample it",
              blob_id, desc->meta.BindFlags);

   info.modifier = desc->drmFormatModifier;
   info.allocation_size = desc->allocationSize;
   info.plane_count = desc->planeCount;
   info.texture_layout = desc->meta.TextureLayout;
   for (uint32_t i = 0; i < desc->planeCount; i++) {
      info.planes[i].offset = desc->planes[i].offset;
      info.planes[i].pitch = desc->planes[i].pitch;
   }

   const int export_fd = desc->fd;
   const uint64_t export_size = desc->allocationSize;
#endif

   /* The channel order the exporter actually used.  An importer cannot
    * derive it: DRI3 carries no fourcc, so it can only guess from
    * depth/bpp and always guesses the screen visual's BGRA. */
   const uint32_t export_virgl_format =
      npt_shared_dxgi_to_virgl_format(NPT_SHARED_EXPORT_FORMAT(desc));

   /* Publish the export-level facts into the exporter's shmem window. */
   struct npt_resource *data_res = npt_context_get_resource(ctx, data_res_id);
   if (!data_res || data_res->fd_type != VIRGL_RESOURCE_FD_SHM ||
       !data_res->u.data) {
      npt_log("shared: export: data resource %u not found", data_res_id);
      return NPT_E_INVALIDARG;
   }

   /* data_off is guest-supplied: bound the write to the mapping. */
   if ((uint64_t)data_off + sizeof(info) > data_res->size) {
      npt_log("shared: export: data_off=%u overruns res size=%zu",
              data_off, data_res->size);
      return NPT_E_INVALIDARG;
   }
   memcpy((uint8_t *)data_res->u.data + data_off, &info, sizeof(info));

   /* Stage the pending blob the guest KMD claims via
    * RESOURCE_CREATE_BLOB(HOST3D, blob_id).  The table takes fd
    * ownership; the texture keeps its own. */
   int fd = dup(export_fd);
   if (fd < 0) {
      npt_log("shared: export: blob_id %" PRIu64 " dup failed", blob_id);
      return NPT_E_FAIL;
   }
   if (!npt_context_register_pending_blob(ctx, blob_id,
                                          NPT_SHARED_FD_TYPE, fd,
                                          export_size,
                                          export_virgl_format)) {
      close(fd);
      return NPT_E_FAIL;
   }

#ifdef __APPLE__
   npt_log("shared: exported blob_id=%" PRIu64 " %ux%u fmt=%u pitch=%" PRIu64
           " (ctx %u)", blob_id, desc->width, desc->height, desc->dxgi_format,
           desc->stride, ctx->ctx_id);
#else
   npt_log("shared: exported blob_id=%" PRIu64 " %ux%u fmt=%u mod=0x%016"
           PRIx64 " pitch=%" PRIu64 " (ctx %u)", blob_id, desc->meta.Width,
           desc->meta.Height, desc->meta.Format, desc->drmFormatModifier,
           desc->planes[0].pitch, ctx->ctx_id);
#endif
   return NPT_S_OK;
}

HRESULT
npt_shared_open_res(struct npt_context *ctx, uint64_t device_id,
                    const struct npt_cmd_shared_open_res *cmd)
{
   if (!device_id || !cmd->res_id || !cmd->mint_object_id)
      return NPT_E_INVALIDARG;
   if (cmd->export_info.plane_count < 1 ||
       cmd->export_info.plane_count > NPT_BLOB_EXPORT_MAX_PLANES)
      return NPT_E_INVALIDARG;

   void *device = npt_context_lookup_object(ctx, NULL, device_id,
                                            NPT_OBJECT_TYPE_ID3D11DEVICE);
   if (!device) {
      npt_log("shared: open: device id 0x%016" PRIx64 " not found", device_id);
      return NPT_E_INVALIDARG;
   }

   /* Wait for the attach-forwarded resource.  Same-context opens hit
    * immediately (the blob create recorded it). */
   struct npt_resource *res = NULL;
   for (int waited_ms = 0;; waited_ms += NPT_SHARED_ATTACH_POLL_MS) {
      res = npt_context_get_resource(ctx, cmd->res_id);
      if (res || waited_ms >= NPT_SHARED_ATTACH_WAIT_MS)
         break;
      thrd_sleep(&(struct timespec){
                    .tv_nsec = NPT_SHARED_ATTACH_POLL_MS * 1000000L }, NULL);
   }
   if (!res || res->fd_type != NPT_SHARED_FD_TYPE || res->u.fd < 0) {
      npt_log("shared: open: res_id %u not attached (found=%d type=%d)",
              cmd->res_id, res != NULL, res ? (int)res->fd_type : -1);
      return NPT_E_INVALIDARG;
   }

   /* Rebuild the exporter's descriptor around our own fd reference. */
#ifdef __APPLE__
   struct npt_darwin_shared_texture desc;
   memset(&desc, 0, sizeof(desc));
   desc.magic = NPT_DARWIN_SHARED_TEXTURE_MAGIC;
   desc.version = NPT_DARWIN_SHARED_HANDLE_VERSION;
   desc.offset = cmd->export_info.planes[0].offset;
   desc.width = cmd->width;
   desc.height = cmd->height;
   desc.dxgi_format = cmd->format;
   desc.mip_levels = cmd->mip_levels;
   desc.array_size = cmd->array_size;
   desc.sample_count = cmd->sample_count;
   desc.bind_flags = cmd->bind_flags;
   desc.misc_flags = cmd->misc_flags;
   desc.cpu_access = cmd->cpu_access_flags;
   /* One linear plane of shared memory; usage/layout/modifier from the
    * wire have no darwin-backend equivalent. */
   desc.stride = cmd->export_info.planes[0].pitch;
   desc.size = cmd->export_info.allocation_size;
#else
   struct DxvkSharedTextureDescriptor desc;
   memset(&desc, 0, sizeof(desc));
   desc.magic = DXVK_SHARED_DESCRIPTOR_TEXTURE;
   desc.version = DXVK_SHARED_DESCRIPTOR_VERSION;
   desc.structSize = sizeof(desc);
   desc.meta.Width = cmd->width;
   desc.meta.Height = cmd->height;
   desc.meta.MipLevels = cmd->mip_levels;
   desc.meta.ArraySize = cmd->array_size;
   desc.meta.Format = cmd->format;
   desc.meta.SampleDesc.Count = cmd->sample_count;
   desc.meta.SampleDesc.Quality = 0;
   desc.meta.Usage = cmd->usage;
   desc.meta.BindFlags = cmd->bind_flags;
   desc.meta.CPUAccessFlags = cmd->cpu_access_flags;
   desc.meta.MiscFlags = cmd->misc_flags;
   desc.meta.TextureLayout = cmd->export_info.texture_layout;
   desc.drmFormatModifier = cmd->export_info.modifier;
   desc.planeCount = cmd->export_info.plane_count;
   for (uint32_t i = 0; i < desc.planeCount; i++) {
      desc.planes[i].offset = cmd->export_info.planes[i].offset;
      desc.planes[i].pitch = cmd->export_info.planes[i].pitch;
   }
   desc.allocationSize = cmd->export_info.allocation_size;
#endif

   /* The import dup()s the fd internally; hold our own reference so a
    * concurrent resource destroy can't invalidate res->u.fd mid-call. */
   desc.fd = dup(res->u.fd);
   if (desc.fd < 0)
      return NPT_E_FAIL;

   PFN_ID3D11Device_OpenSharedResource open_shared =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Device_OpenSharedResource,
                        npt_com_vtable(device),
                        NPT_VTBL_ID3D11Device_OpenSharedResource);

   void *texture = NULL;
   HRESULT hr = open_shared(device, (HANDLE)(uintptr_t)&desc,
                            &NPT_IID_ID3D11Texture2D, &texture);
   close(desc.fd);

   if (NPT_FAILED(hr) || !texture) {
      npt_log("shared: open: res_id %u import failed (hr=0x%x)",
              cmd->res_id, hr);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   /* The freshly imported texture carries one reference; the object
    * table registration is what the guest's minted id releases. */
   npt_context_register_object(ctx, cmd->mint_object_id, texture,
                               NPT_OBJECT_TYPE_ID3D11TEXTURE2D);

   npt_log("shared: opened res_id=%u -> id 0x%016" PRIx64 " (ctx %u)",
           cmd->res_id, cmd->mint_object_id, ctx->ctx_id);
   return NPT_S_OK;
}
