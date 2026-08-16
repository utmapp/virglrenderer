/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared declarations and inline helpers for the Neptune host runtime.
 */

#ifndef NPT_COMMON_H
#define NPT_COMMON_H

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "c11/threads.h"
#include "util/hash_table.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/os_file.h"
#include "util/os_misc.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_pointer.h"
#include "util/u_thread.h"
#include "virgl_context.h"
#include "virgl_util.h"
#include "virglrenderer.h"

#include "npt_renderer.h"

#define NPT_DEBUG(category) (unlikely(npt_debug_flags & NPT_DEBUG_##category))

/* Caller must validate the result with npt_region_is_valid to catch overflow. */
#define NPT_REGION_INIT(offset, size)                                                    \
   {                                                                                     \
      .begin = (offset), .end = (offset) + (size)                                        \
   }

struct npt_context;
struct npt_ring;

typedef uint64_t npt_object_id;

enum npt_debug_flags {
   NPT_DEBUG_PROFILE     = 1 << 0,
   /* Per-ring sync-queue trace: one NPT-Q-RETIRE line per queue worker
    * poll completion (with the actual poll_us).  Attributes guest-side
    * wsi_us stalls to real GPU work vs virtio fence propagation. */
   NPT_DEBUG_FENCE_TRACE = 1 << 1,
};

/* uint16_t (not enum) so the protocol generator can emit new tags
 * without touching this header.  Concrete values live in
 * neptune-protocol/npt_protocol_defs.h, pinned by npt_interface_ids.json. */
typedef uint16_t npt_object_type;

struct npt_region {
   size_t begin;
   size_t end;
};

extern uint32_t npt_debug_flags;

/* Periodic profiler dump cadence, parsed from NPT_PROFILE_PERIOD_MS.
 * Only consulted when NPT_DEBUG=profile is set. */
extern uint32_t npt_profile_period_ms;

void
npt_debug_init(void);

/* Capset answers a split deployment has to supply by hand, since the
 * process filling the capset cannot observe the render server's backend.
 * Negative when unset: NPT_CAPSET_D3D12 is a 0/1 standing in for the
 * D3D12 probe, NPT_CAPSET_CAPS the whole caps word. */
long
npt_capset_d3d12_override(void);

long
npt_capset_caps_override(void);

void
npt_log(const char *fmt, ...);

static inline bool
npt_region_is_valid(const struct npt_region *region)
{
   return region->begin <= region->end;
}

static inline size_t
npt_region_size(const struct npt_region *region)
{
   return region->end - region->begin;
}

static inline bool
npt_region_is_aligned(const struct npt_region *region, size_t align)
{
   assert(util_is_power_of_two_nonzero(align));
   return !((region->begin | region->end) & (align - 1));
}

static inline bool
npt_region_is_disjoint(const struct npt_region *region, const struct npt_region *other)
{
   return region->begin >= other->end || region->end <= other->begin;
}

static inline bool
npt_region_is_within(const struct npt_region *region, const struct npt_region *other)
{
   return region->begin >= other->begin && region->end <= other->end;
}

static inline struct npt_region
npt_region_make_relative(const struct npt_region *region)
{
   return (struct npt_region){
      .end = region->end - region->begin,
   };
}

/* a >= b, handling 32-bit wraparound. */
static inline bool
npt_seqno_ge(uint32_t a, uint32_t b)
{
   return (a - b) <= INT32_MAX;
}

#endif /* NPT_COMMON_H */
