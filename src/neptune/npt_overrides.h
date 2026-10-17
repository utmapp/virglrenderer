/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_OVERRIDES_H
#define NPT_OVERRIDES_H

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

/* Non-const because struct npt_dispatch_context expects non-const
 * pointers.  Initialised once at startup, read-only thereafter. */
extern struct npt_dispatch_toplevel_overrides npt_toplevel_overrides;

/* Shared-HANDLE rejection.  Each table sets only the shared-HANDLE
 * methods; default dispatch handles the rest. */
extern struct npt_dispatch_idxgiresource_overrides npt_idxgiresource_overrides;
extern struct npt_dispatch_idxgiresource1_overrides npt_idxgiresource1_overrides;
extern struct npt_dispatch_id3d11device_overrides npt_id3d11device_overrides;
extern struct npt_dispatch_id3d11device1_overrides npt_id3d11device1_overrides;
extern struct npt_dispatch_id3d11device5_overrides npt_id3d11device5_overrides;
extern struct npt_dispatch_id3d11fence_overrides npt_id3d11fence_overrides;

/* ID3D12Device: the shared-HANDLE rejections plus the FEATURE_LEVELS blob
 * repack (npt_overrides_d3d12_device.c). */
extern struct npt_dispatch_id3d12device_overrides npt_id3d12device_overrides;

/* DXR state objects (npt_overrides_d3d12_raytracing.c): the wire deep-copies
 * SUBOBJECT_TO_EXPORTS_ASSOCIATION.pSubobjectToAssociate, so it is relinked
 * to the decoded subobject array before the backend sees the desc. */
extern struct npt_dispatch_id3d12device5_overrides npt_id3d12device5_overrides;
extern struct npt_dispatch_id3d12device7_overrides npt_id3d12device7_overrides;

/* Hooks Begin/End on the immediate context to maintain the per-query
 * pending list. */
extern struct npt_dispatch_id3d11devicecontext_overrides
   npt_query_dc_overrides;

/* Fence-Signal hook lives on DC4 because D3D11 has no
 * ID3D11Fence::Signal. */
extern struct npt_dispatch_id3d11devicecontext4_overrides
   npt_fence_dc4_overrides;

/* D3D12 fence-feedback Signal hooks (npt_overrides_d3d12_fence.c). */
extern struct npt_dispatch_id3d12commandqueue_overrides
   npt_id3d12commandqueue_overrides;
extern struct npt_dispatch_id3d12fence_overrides
   npt_id3d12fence_overrides;

#endif /* NPT_OVERRIDES_H */
