/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Neptune initialization and context creation tests.
 * These test the protocol/context layer without requiring real D3D libraries.
 * Uses a simple assert-based test harness to avoid dependency on libcheck.
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <virglrenderer.h>
#include "virgl_hw.h"
#include "drm/drm-uapi/virtgpu_drm.h"
#include "neptune/neptune-protocol/npt_protocol_defs.h"
#include "neptune_hw.h"

static struct virgl_renderer_callbacks test_cbs;
static int cookie;
static int tests_run = 0;
static int tests_failed = 0;

#define RUN_TEST(fn)                                     \
   do {                                                  \
      tests_run++;                                       \
      printf("  %-50s ", #fn);                           \
      fflush(stdout);                                    \
      if (fn()) {                                        \
         printf("PASS\n");                               \
      } else {                                           \
         printf("FAIL\n");                               \
         tests_failed++;                                 \
      }                                                  \
   } while (0)

#define EXPECT(cond)                                     \
   do {                                                  \
      if (!(cond)) {                                     \
         fprintf(stderr, "    EXPECT failed: %s:%d: %s\n", \
                 __FILE__, __LINE__, #cond);             \
         return 0;                                       \
      }                                                  \
   } while (0)

static void
setup(void)
{
   test_cbs.version = 1;
   /* Neptune is render-server-only.  RENDER_SERVER_EXEC_PATH is set by
    * meson to the in-tree binary so the test runs out of the build
    * tree without `ninja install`. */
   int r = virgl_renderer_init(&cookie,
      VIRGL_RENDERER_NEPTUNE | VIRGL_RENDERER_NO_VIRGL |
      VIRGL_RENDERER_RENDER_SERVER, &test_cbs);
   assert(r == 0);
   (void)r;
}

static void
teardown(void)
{
   virgl_renderer_cleanup(&cookie);
}

static int
test_neptune_init_ok(void)
{
   setup();
   /* If we get here, init succeeded */
   teardown();
   return 1;
}

static int
test_neptune_get_capset(void)
{
   setup();

   uint32_t max_ver, max_size;
   virgl_renderer_get_cap_set(VIRTGPU_DRM_CAPSET_NEPTUNE, &max_ver, &max_size);
   EXPECT(max_ver == 0);
   EXPECT(max_size == sizeof(struct virgl_renderer_capset_neptune));

   struct virgl_renderer_capset_neptune caps;
   memset(&caps, 0xff, sizeof(caps));
   virgl_renderer_fill_caps(VIRTGPU_DRM_CAPSET_NEPTUNE, 0, &caps);
   /* fill_caps must advertise exactly what the generated protocol
    * headers define -- a mismatch means the capset plumbing is stale. */
   EXPECT(caps.wire_format_version == NPT_PROTOCOL_WIRE_VERSION);

   /* Backend-capability bits derive from NPT_BACKEND: the DXMT set for
    * "dxmt", DXIL alone for d3dmetal (the default).  The D3D12 bit comes
    * from a dlopen probe that varies by host, so it is masked out. */
   const uint32_t dxmt_bits =
      VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_TBDR |
      VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_MSAA_RTV_FORCED_SC1 |
      VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_MAP_DEFAULT_BUFFERS |
      VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_SHADER_CACHE;
   const uint32_t backend_bits = dxmt_bits |
      VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_EXTENDED_RESOURCE_SHARING |
      VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_DXIL;
   EXPECT((caps.caps_flags & backend_bits) ==
          VIRGL_RENDERER_CAPSET_NEPTUNE_CAP_DXIL); /* default = d3dmetal */

   setenv("NPT_BACKEND", "dxmt", 1);
   virgl_renderer_fill_caps(VIRTGPU_DRM_CAPSET_NEPTUNE, 0, &caps);
   EXPECT((caps.caps_flags & backend_bits) == dxmt_bits);
   unsetenv("NPT_BACKEND");

   /* NPT_CAPSET_CAPS overrides the whole word. */
   setenv("NPT_CAPSET_CAPS", "0x22", 1);
   virgl_renderer_fill_caps(VIRTGPU_DRM_CAPSET_NEPTUNE, 0, &caps);
   EXPECT(caps.caps_flags == 0x22u);
   unsetenv("NPT_CAPSET_CAPS");

   teardown();
   return 1;
}

static int
test_neptune_create_context(void)
{
   setup();

   const char *name = "test-neptune";
   int ret = virgl_renderer_context_create_with_flags(
      1, VIRTGPU_DRM_CAPSET_NEPTUNE, strlen(name), name);
   EXPECT(ret == 0);

   virgl_renderer_context_destroy(1);
   teardown();
   return 1;
}

/* Re-creating an existing (id, capset) is a no-op (idempotent lookup). */
static int
test_neptune_create_context_duplicate(void)
{
   setup();

   const char *name = "dup";
   int ret = virgl_renderer_context_create_with_flags(
      1, VIRTGPU_DRM_CAPSET_NEPTUNE, strlen(name), name);
   EXPECT(ret == 0);

   ret = virgl_renderer_context_create_with_flags(
      1, VIRTGPU_DRM_CAPSET_NEPTUNE, strlen(name), name);
   EXPECT(ret == 0);

   virgl_renderer_context_destroy(1);
   teardown();
   return 1;
}

static int
test_neptune_destroy_unknown_context(void)
{
   setup();

   /* No-op for an id that was never created. */
   virgl_renderer_context_destroy(42);

   teardown();
   return 1;
}

static int
test_neptune_create_multiple_contexts(void)
{
   setup();

   const char *name1 = "test-npt-1";
   const char *name2 = "test-npt-2";

   int ret;
   ret = virgl_renderer_context_create_with_flags(
      1, VIRTGPU_DRM_CAPSET_NEPTUNE, strlen(name1), name1);
   EXPECT(ret == 0);

   ret = virgl_renderer_context_create_with_flags(
      2, VIRTGPU_DRM_CAPSET_NEPTUNE, strlen(name2), name2);
   EXPECT(ret == 0);

   virgl_renderer_context_destroy(2);
   virgl_renderer_context_destroy(1);
   teardown();
   return 1;
}

static int
test_neptune_context_create_destroy_cycle(void)
{
   setup();

   for (int i = 0; i < 10; i++) {
      const char *name = "cycle-test";
      int ret = virgl_renderer_context_create_with_flags(
         1, VIRTGPU_DRM_CAPSET_NEPTUNE, strlen(name), name);
      EXPECT(ret == 0);
      virgl_renderer_context_destroy(1);
   }

   teardown();
   return 1;
}

int
main(void)
{
   printf("Neptune init tests:\n");

   RUN_TEST(test_neptune_init_ok);
   RUN_TEST(test_neptune_get_capset);
   RUN_TEST(test_neptune_create_context);
   RUN_TEST(test_neptune_create_context_duplicate);
   RUN_TEST(test_neptune_destroy_unknown_context);
   RUN_TEST(test_neptune_create_multiple_contexts);
   RUN_TEST(test_neptune_context_create_destroy_cycle);

   printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
   return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
