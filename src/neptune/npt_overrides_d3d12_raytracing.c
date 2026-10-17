/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXR state-object dispatch overrides.
 *
 * D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION names the subobject it associates by
 * POINTER into the same D3D12_STATE_OBJECT_DESC's subobject array, and the
 * runtime (and D3DMetal) resolve it by pointer identity.  The wire carries
 * the pointee as a deep copy, so after decoding the pointer names a private
 * copy that is not an element of pSubobjects and every association silently
 * resolves to nothing (local root signatures and shader configs lost).
 *
 * Relink each association to the decoded subobject whose type and desc
 * contents equal the copy's.  Desc contents are value types after decoding
 * (interface pointers are already the host objects), so equal contents mean
 * the same subobject; two identical candidates are interchangeable.
 */

#include <string.h>

#include "npt_common.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_id3d12device.h"

/* Bytes of a subobject desc that identify it, by subobject type.  0: not
 * an association target the guest emits (libraries, collections, hit
 * groups, associations), compared by pointer only. */
static size_t
npt_subobject_desc_size(D3D12_STATE_SUBOBJECT_TYPE type)
{
   switch (type) {
   case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
      return sizeof(D3D12_STATE_OBJECT_CONFIG);
   case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
      return sizeof(D3D12_GLOBAL_ROOT_SIGNATURE);
   case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
      return sizeof(D3D12_LOCAL_ROOT_SIGNATURE);
   case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
      return sizeof(D3D12_NODE_MASK);
   case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
      return sizeof(D3D12_RAYTRACING_SHADER_CONFIG);
   case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
      return sizeof(D3D12_RAYTRACING_PIPELINE_CONFIG);
   case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
      return sizeof(D3D12_RAYTRACING_PIPELINE_CONFIG1);
   default:
      return 0;
   }
}

static void
npt_relink_associations(const D3D12_STATE_OBJECT_DESC *desc)
{
   if (!desc || !desc->pSubobjects)
      return;
   D3D12_STATE_SUBOBJECT *subs = (D3D12_STATE_SUBOBJECT *)desc->pSubobjects;
   for (UINT i = 0; i < desc->NumSubobjects; i++) {
      if (subs[i].Type != D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION ||
          !subs[i].pDesc)
         continue;
      D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *a =
         (D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *)subs[i].pDesc;
      const D3D12_STATE_SUBOBJECT *copy = a->pSubobjectToAssociate;
      if (!copy)
         continue;
      /* Already an element of the array: nothing to do. */
      if (copy >= subs && copy < subs + desc->NumSubobjects)
         continue;
      const size_t sz = npt_subobject_desc_size(copy->Type);
      const D3D12_STATE_SUBOBJECT *match = NULL;
      for (UINT j = 0; j < desc->NumSubobjects && !match; j++) {
         if (subs[j].Type != copy->Type)
            continue;
         if (subs[j].pDesc == copy->pDesc ||
             (sz && subs[j].pDesc && copy->pDesc &&
              !memcmp(subs[j].pDesc, copy->pDesc, sz)))
            match = &subs[j];
      }
      if (match)
         a->pSubobjectToAssociate = match;
      else
         npt_log("CreateStateObject: association %u (subobject type %u) has no "
                 "matching subobject; left unlinked", i, (unsigned)copy->Type);
   }
}

/* D3DMetal reads a DXIL library's RDAT part to find its functions and
 * crashes (NULL deref in CreateDXILContainer) when it is missing -- e.g.
 * when a guest driver forwards only the DXIL program part.  Refuse such a
 * desc instead of taking the whole render server down. */
static bool
npt_libraries_have_rdat(const D3D12_STATE_OBJECT_DESC *d)
{
   if (!d)
      return true;
   for (UINT i = 0; i < d->NumSubobjects; i++) {
      const D3D12_STATE_SUBOBJECT *so = &d->pSubobjects[i];
      if (so->Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !so->pDesc)
         continue;
      const D3D12_DXIL_LIBRARY_DESC *lib = so->pDesc;
      const uint8_t *b = lib->DXILLibrary.pShaderBytecode;
      const size_t cb = lib->DXILLibrary.BytecodeLength;
      bool found = false;
      if (b && cb >= 32 && memcmp(b, "DXBC", 4) == 0) {
         uint32_t n;
         memcpy(&n, b + 28, 4);
         for (uint32_t k = 0; k < n && 32 + 4 * (size_t)k + 4 <= cb; k++) {
            uint32_t off;
            memcpy(&off, b + 32 + 4 * k, 4);
            if ((size_t)off + 4 <= cb && memcmp(b + off, "RDAT", 4) == 0)
               found = true;
         }
      }
      if (!found) {
         npt_log("CreateStateObject: DXIL library %u (%zu bytes) has no RDAT "
                 "part; refusing it (D3DMetal would crash)", i, cb);
         return false;
      }
   }
   return true;
}

static HRESULT
npt_override_ID3D12Device5_CreateStateObject(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device5_CreateStateObject *args,
   PFN_ID3D12Device5_CreateStateObject original)
{
   npt_relink_associations(args->pDesc);
   if (!npt_libraries_have_rdat(args->pDesc)) {
      if (args->ppStateObject)
         *args->ppStateObject = NULL;
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }
   args->ret = original(args->_self, args->pDesc, args->riid, args->ppStateObject);
   if (args->ret < 0)
      npt_log("CreateStateObject(type %u, %u subobjects) FAILED 0x%08x",
              args->pDesc ? (unsigned)args->pDesc->Type : 0u,
              args->pDesc ? args->pDesc->NumSubobjects : 0u, (unsigned)args->ret);
   return args->ret;
}

static HRESULT
npt_override_ID3D12Device7_AddToStateObject(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D12Device7_AddToStateObject *args,
   PFN_ID3D12Device7_AddToStateObject original)
{
   npt_relink_associations(args->pAddition);
   if (!npt_libraries_have_rdat(args->pAddition)) {
      if (args->ppNewStateObject)
         *args->ppNewStateObject = NULL;
      args->ret = NPT_E_INVALIDARG;
      return args->ret;
   }
   args->ret = original(args->_self, args->pAddition, args->pStateObjectToGrowFrom,
                        args->riid, args->ppNewStateObject);
   return args->ret;
}

struct npt_dispatch_id3d12device5_overrides npt_id3d12device5_overrides = {
   .CreateStateObject = npt_override_ID3D12Device5_CreateStateObject,
};

struct npt_dispatch_id3d12device7_overrides npt_id3d12device7_overrides = {
   .AddToStateObject = npt_override_ID3D12Device7_AddToStateObject,
};
