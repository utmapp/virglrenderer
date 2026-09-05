/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host-side ID3D11DeviceContext::End hook that counts the End and
 * queues the query's feedback entry for the next poll.  Begin and
 * GetData need no host hook: the guest counts Ends the same way and
 * clears the slot under its new count before sending each End, so a
 * stale-flag observation hits a version mismatch (S_FALSE), and the
 * guest's local GetData reads the feedback slot directly.
 */

#include "npt_context.h"
#include "npt_feedback.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d11devicecontext.h"

static void
npt_override_DC_End(struct npt_dispatch_context *dctx,
                    struct npt_command_ID3D11DeviceContext_End *args,
                    PFN_ID3D11DeviceContext_End original)
{
   original(args->_self, args->pAsync);

   /* args->pAsync is the host pointer; the registry indexes by
    * guest-allocated id, so mark_end walks the registry once to
    * locate the matching entry.  Linear in registered query count,
    * which is small in practice. */
   struct npt_context *ctx = npt_context_from_dispatch(dctx);
   if (!ctx)
      return;
   npt_feedback_query_mark_end(ctx, args->_self, args->pAsync);
}

/* A wire GetData answers one query straight from the host library.
 * D3D completes queries in End order, so once it reports S_OK every
 * query ended before it is complete too, and the guest may read their
 * slots right after this reply: publish them now instead of leaving
 * them to the next rate-limited poll. */
static HRESULT
npt_override_DC_GetData(struct npt_dispatch_context *dctx,
                        struct npt_command_ID3D11DeviceContext_GetData *args,
                        PFN_ID3D11DeviceContext_GetData original)
{
   HRESULT hr = original(args->_self, args->pAsync, args->pData,
                         args->DataSize, args->GetDataFlags);
   if (hr == 0 /* S_OK */) {
      struct npt_context *ctx = npt_context_from_dispatch(dctx);
      if (ctx)
         npt_feedback_poll_interval(ctx, 0);
   }
   return hr;
}

struct npt_dispatch_id3d11devicecontext_overrides
npt_query_dc_overrides = {
   .End = npt_override_DC_End,
   .GetData = npt_override_DC_GetData,
};
