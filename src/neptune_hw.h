/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NEPTUNE_HW_H
#define NEPTUNE_HW_H

#include <stdint.h>

struct virgl_renderer_capset_neptune {
   uint32_t wire_format_version;
   /* VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_* bits.  Guests predating the
    * field ignore it; hosts predating it zero the whole struct, so a
    * clear bit always means "not supported". */
   uint32_t caps_flags;
   uint32_t pad[13]; /* reserved for future use */
};

/* The host has a D3D12 backend, so D3D12CreateDevice can succeed.  A
 * clear bit means the guest must fail device creation locally rather
 * than discover it through a failed wire call. */
#define VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_D3D12 (1u << 0)

/* Adapter-scope capabilities that differ by host backend; the guest
 * D3D11 UMD folds them into its own cap answers.  TBDR is Metal on Apple
 * silicon either way, but D3DMetal presents itself as immediate-mode. */
#define VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_TBDR                        (1u << 1)
#define VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_MSAA_RTV_FORCED_SC1         (1u << 2)
#define VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_EXTENDED_RESOURCE_SHARING   (1u << 3)
#define VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_MAP_DEFAULT_BUFFERS         (1u << 4)
#define VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_SHADER_CACHE                (1u << 5)

/* The D3D12 backend consumes DXIL containers directly (SM 6.x).  Set for
 * D3DMetal (Metal Shader Converter's native input is DXIL); clear for
 * DXMT, whose shader front end parses DXBC only. */
#define VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_DXIL                        (1u << 6)

#endif /* NEPTUNE_HW_H */
