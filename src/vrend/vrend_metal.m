/*
 * Copyright 2025 Turing Software, LLC
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "virglrenderer.h"
#include "vrend_metal.h"
#include "pipe/p_state.h"
#include "util/u_format.h"
#include "util/u_math.h"
#include <sys/mman.h>
#include <unistd.h>
#include <Metal/Metal.h>

struct metal_format_conversion {
   uint32_t virgl_format;
   MTLPixelFormat metal_format;
};

static bool virgl_format_to_metal_format(uint32_t format, MTLPixelFormat *metal_format)
{
   static const struct metal_format_conversion conversions[] = {
      { VIRGL_FORMAT_R8G8B8A8_UNORM, MTLPixelFormatRGBA8Unorm },
      { VIRGL_FORMAT_R8G8B8X8_UNORM, MTLPixelFormatRGBA8Unorm },
      { VIRGL_FORMAT_R8G8B8A8_SRGB, MTLPixelFormatRGBA8Unorm_sRGB },
      { VIRGL_FORMAT_R8G8B8X8_SRGB, MTLPixelFormatRGBA8Unorm_sRGB },
      { VIRGL_FORMAT_B8G8R8A8_UNORM, MTLPixelFormatBGRA8Unorm },
      { VIRGL_FORMAT_B8G8R8X8_UNORM, MTLPixelFormatBGRA8Unorm },
      { VIRGL_FORMAT_B8G8R8A8_SRGB, MTLPixelFormatBGRA8Unorm_sRGB },
      { VIRGL_FORMAT_B8G8R8X8_SRGB, MTLPixelFormatBGRA8Unorm_sRGB },
      { VIRGL_FORMAT_R16G16B16A16_FLOAT, MTLPixelFormatRGBA16Float },
      { VIRGL_FORMAT_R16G16B16X16_FLOAT, MTLPixelFormatRGBA16Float },
      { VIRGL_FORMAT_R16G16B16A16_UNORM, MTLPixelFormatRGBA16Unorm },
      { VIRGL_FORMAT_R16G16B16X16_UNORM, MTLPixelFormatRGBA16Unorm },
      { VIRGL_FORMAT_R32G32B32A32_FLOAT, MTLPixelFormatRGBA32Float },
      { VIRGL_FORMAT_R10G10B10A2_UNORM, MTLPixelFormatRGB10A2Unorm },
      { VIRGL_FORMAT_R10G10B10X2_UNORM, MTLPixelFormatRGB10A2Unorm },
      { VIRGL_FORMAT_B10G10R10A2_UNORM, MTLPixelFormatBGR10A2Unorm },
      { VIRGL_FORMAT_B10G10R10X2_UNORM, MTLPixelFormatBGR10A2Unorm },
      { VIRGL_FORMAT_R8_UNORM, MTLPixelFormatR8Unorm },
      { VIRGL_FORMAT_R16_UNORM, MTLPixelFormatR16Unorm },
      { VIRGL_FORMAT_R8G8_UNORM, MTLPixelFormatRG8Unorm },
      { VIRGL_FORMAT_R16G16_UNORM, MTLPixelFormatRG16Unorm },
      { VIRGL_FORMAT_B5G6R5_UNORM, MTLPixelFormatB5G6R5Unorm },
      // { VIRGL_FORMAT_NV12, }, /* Conversion not possible */
      // { VIRGL_FORMAT_YV12, }, /* Conversion not possible */
   };

   for (uint32_t i = 0; i < ARRAY_SIZE(conversions); i++) {
      if (conversions[i].virgl_format == format) {
         *metal_format = conversions[i].metal_format;
         return true;
      }
   }

   return false;
}

static MTLTextureUsage virgl_bind_to_metal_usage_flags(uint32_t flags)
{
   MTLTextureUsage ret = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

   if (flags & PIPE_BIND_RENDER_TARGET)
      ret |= MTLTextureUsageRenderTarget;
   if (flags & PIPE_BIND_DEPTH_STENCIL)
      ret |= MTLTextureUsageRenderTarget;

   return ret;
}

static MTLResourceOptions virgl_usage_to_metal_resource_options(uint32_t usage)
{
   switch (usage) {
   case PIPE_USAGE_DEFAULT:
   case PIPE_USAGE_STAGING:
   default:
      return MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;
   case PIPE_USAGE_IMMUTABLE:
      return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined | MTLHazardTrackingModeUntracked;
   case PIPE_USAGE_DYNAMIC:
   case PIPE_USAGE_STREAM:
      return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
   }
}

static MTLTextureDescriptor *new_descriptor(const struct vrend_metal_texture_description *desc)
{
   MTLPixelFormat pixel_format;

   if (!virgl_format_to_metal_format(desc->format, &pixel_format)) {
      return NULL;
   }

   MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
   descriptor.textureType = MTLTextureType2D;
   descriptor.pixelFormat = pixel_format;
   descriptor.width = desc->width;
   descriptor.height = desc->height;
   descriptor.resourceOptions = virgl_usage_to_metal_resource_options(desc->usage);
   descriptor.usage = virgl_bind_to_metal_usage_flags(desc->bind);
   if (desc->usage == PIPE_USAGE_IMMUTABLE) {
      descriptor.usage &= ~MTLTextureUsageShaderWrite;
   }

   return descriptor;
}

bool virgl_metal_create_texture(MTLDevice_id device,
                                const struct vrend_metal_texture_description *desc,
                                MTLTexture_id *tex)
{
   id<MTLDevice> mtl_device = (id<MTLDevice>)device;
   MTLTextureDescriptor *descriptor = new_descriptor(desc);
   if (descriptor) {
      *tex = [mtl_device newTextureWithDescriptor:descriptor];
      [descriptor release];
      return true;
   }

   return false;
}

bool virgl_metal_create_texture_from_shm(MTLDevice_id device,
                                         int fd,
                                         size_t size,
                                         const struct vrend_metal_texture_description *desc,
                                         MTLTexture_id *tex)
{
   id<MTLDevice> mtl_device = (id<MTLDevice>)device;
   MTLTextureDescriptor *descriptor = new_descriptor(desc);
   *tex = nil;
   if (!mtl_device || !descriptor) {
      [descriptor release];
      return false;
   }

   NSUInteger deviceAlignment, bytesPerRow;
   /* the backing MTLBuffer is shared system memory, so the texture must be too */
   descriptor.resourceOptions = MTLResourceStorageModeShared;
   deviceAlignment = [mtl_device minimumLinearTextureAlignmentForPixelFormat:descriptor.pixelFormat];
   bytesPerRow = align(desc->stride, deviceAlignment);

   /* newBufferWithBytesNoCopy requires a page-aligned pointer and length */
   const size_t page_size = getpagesize();
   const size_t map_size = (size + page_size - 1) & ~(page_size - 1);

   /* Metal asserts, aborting the whole process, on a linear texture whose
    * rows or extent do not fit its buffer.  The layout comes from the guest,
    * so refuse it here instead. */
   const uint64_t min_row = util_format_get_stride(desc->format, desc->width);
   if (!desc->width || !desc->height || !min_row || bytesPerRow < min_row ||
       desc->offset % deviceAlignment ||
       desc->offset + (uint64_t)bytesPerRow * (desc->height - 1) + min_row > map_size) {
      [descriptor release];
      return false;
   }

   void *shm_ptr = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (shm_ptr == MAP_FAILED) {
      [descriptor release];
      return false;
   }

   /* wrap the shared memory as an MTLBuffer; the deallocator unmaps it once the
    * buffer (retained by the texture and, in turn, by ANGLE) is released.
    */
   id<MTLBuffer> mtl_buffer =
      [mtl_device newBufferWithBytesNoCopy:shm_ptr
                                    length:map_size
                                   options:MTLResourceStorageModeShared
                               deallocator:^(void *ptr, NSUInteger length) {
                                  munmap(ptr, length);
                               }];
   if (mtl_buffer) {
      *tex = [mtl_buffer newTextureWithDescriptor:descriptor
                                           offset:desc->offset
                                      bytesPerRow:bytesPerRow];
      [mtl_buffer release];
   } else {
      munmap(shm_ptr, map_size);
   }
   [descriptor release];
   return !!*tex;
}

MTLTexture_id virgl_metal_retain_texture(MTLTexture_id tex)
{
   id<MTLTexture> mtl_texture = (id<MTLTexture>)tex;

   return [mtl_texture retain];
}

void virgl_metal_release_texture(MTLTexture_id tex)
{
   id<MTLTexture> mtl_texture = (id<MTLTexture>)tex;

   [mtl_texture release];
}
