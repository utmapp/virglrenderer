/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Win32 event-HANDLE emulation.  A proxy event is handed to the host
 * D3D library as the HANDLE; when the library signals it, the pollable
 * side wakes the sync-queue worker, which retires the fence the
 * matching ARM_EVENT_FENCE named.
 */

#ifndef NPT_EVENT_H
#define NPT_EVENT_H

#include "npt_common.h"

struct npt_context;

/* Linux uses eventfd (one fd); darwin uses a backend event handle (the
 * HANDLE the host library's SetEvent acts on, with the backend's
 * event_dup_fd providing the pollable side); other POSIX uses pipe2
 * (host writes the signal end, we poll the read end). */
struct npt_event_fd {
#if defined(__linux__)
   int fd;
#elif defined(__APPLE__)
   void *handle;
#else
   int read_fd;
   int write_fd;
#endif
};

struct npt_event_proxy {
   uint64_t token;               /* guest HANDLE = (uintptr_t)HANDLE */
   struct npt_event_fd proxy;
   /* +1 per REGISTER_EVENT, +1 per outstanding ARM.  Proxy stays
    * alive until both drop, so a late SetEvent after RELEASE_EVENT
    * doesn't hit EBADF and the in-flight dup_fd can still observe
    * the signal. */
   uint32_t refcount;
};

struct npt_event_pending_arm {
   uint32_t ring_idx;
   int      dup_fd;         /* pop transfers ownership to the sync queue */
   /* Keeps a refcount on the proxy until pop transfers dup_fd. */
   struct npt_event_proxy *proxy;
   /* NPT_EVENT_ARM_FLAG_AUTO_RELEASE: pop transfers the arm's proxy
    * reference to the sync-queue entry (by pointer) instead of
    * unreffing; the sync queue releases it after the fence retires. */
   bool     auto_release;
   /* GATE_WAIT arms: retire only when the fence truly reaches
    * check_value, re-verified on every wakeup, since the ring's gate
    * event also carries other gates' fires.  check_fence holds an
    * IUnknown reference released at retire. */
   void    *check_fence;
   uint64_t check_value;
   struct list_head head;
};

/* What a pop hands to the sync queue. */
struct npt_event_paired {
   int      fd;             /* dup'd wait fd; ownership transferred */
   /* AUTO_RELEASE arm's transferred proxy reference, released by
    * pointer, not token: a reused token maps to a NEWER proxy in the
    * table by the time the sync queue releases, and a by-token release
    * would drop the new proxy's reference instead. */
   struct npt_event_proxy *release_proxy;
   void    *check_fence;    /* value-gated retirement (or NULL) */
   uint64_t check_value;
};

/* A fence that outran its ARM_EVENT_FENCE (independent channels on the
 * Windows KMD path); parked in ctx->event_pending_fences until the ARM
 * decodes and pairs it. */
struct npt_event_pending_fence {
   struct list_head head;
   uint32_t ring_idx;
   uint64_t fence_id;
};

bool npt_event_init(struct npt_context *ctx);
void npt_event_fini(struct npt_context *ctx);

/* Idempotent: re-register just bumps the refcount. */
void npt_event_register(struct npt_context *ctx, uint64_t token);

/* False if token isn't registered or dup/alloc fails.  arm_flags is
 * NPT_EVENT_ARM_FLAG_* from the wire. */
bool npt_event_arm(struct npt_context *ctx, uint64_t token,
                    uint32_t ring_idx, uint32_t arm_flags);

void npt_event_release(struct npt_context *ctx, uint64_t token);

/* Drop a proxy reference held by pointer (an AUTO_RELEASE arm's
 * transferred reference in a sync-queue entry). */
void npt_event_release_proxy(struct npt_context *ctx,
                             struct npt_event_proxy *pr);

/* Atomic pop-or-park for event-ring fences: returns the duped proxy fd
 * (>= 0, ownership transferred), NPT_EVENT_FENCE_PARKED when the fence
 * outran its ARM and was parked (npt_event_arm will pair and route it),
 * or NPT_EVENT_FENCE_ERR on allocation failure.  out->release_proxy is
 * set to the arm's transferred proxy reference when the arm carried
 * AUTO_RELEASE (the caller must npt_event_release_proxy it after the
 * fence retires), NULL otherwise. */
#define NPT_EVENT_FENCE_PARKED (-2)
#define NPT_EVENT_FENCE_ERR    (-1)
int npt_event_pop_arm_or_park_fence(struct npt_context *ctx,
                                    uint32_t ring_idx, uint64_t fence_id,
                                    struct npt_event_paired *out);

/* GATE_WAIT (D3D12 monitored-fence gates): arm SetEventOnCompletion
 * (fence, value) onto ring_idx's gate event and install a value-gated
 * pending arm.  Takes an IUnknown reference on fence for the sync
 * entry. */
bool npt_event_gate_wait(struct npt_context *ctx, void *fence,
                         uint64_t value, uint32_t ring_idx);

/* D3D12 bridge for GATE_WAIT, so npt_event and npt_queue stay free of
 * D3D COM types. */
void npt_d3d12_gate_addref(void *fence);
void npt_d3d12_gate_release(void *fence);
bool npt_d3d12_gate_seoc(void *fence, uint64_t value, void *signal_handle);
bool npt_d3d12_gate_reached(void *fence, uint64_t value);

/* Context teardown: retire anything still parked. */
void npt_event_drain_parked_fences(struct npt_context *ctx);

/* Release the fences parked on one ring.  Every path that fails to
 * install an arm owes this call: a parked fence has no sync-queue entry,
 * so nothing else -- not even the device-lost budget -- can ever free
 * the guest's wait on it. */
void npt_event_drain_parked_ring(struct npt_context *ctx, uint32_t ring_idx);

/* Drop the pins npt_event_replace_by_token took while decoding the
 * current command.  Call after every dispatched command, on the decode
 * thread that ran it. */
void npt_event_unpin_dispatched(struct npt_context *ctx);

struct npt_dispatch_context;
void *npt_event_replace_by_token(struct npt_dispatch_context *dispatch,
                                  npt_object_id id);

#endif /* NPT_EVENT_H */
