/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hand-written codec (manual_codec) for D3D12_PIPELINE_STATE_STREAM_DESC.
 * Copied verbatim beside the generated headers and included from the
 * structs header, which forward-declares the helpers and defines
 * NPT_CODEC_IS_HOST.
 *
 * The stream is a packed run of records laid out as
 *
 *     struct alignas(void *) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; T payload; };
 *
 * (see the D3D12_PIPELINE_STATE_STREAM_DESC remarks and d3dx12's
 * CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT): a record starts pointer-
 * aligned, its payload at the payload type's alignment.  The walk runs
 * while the offset is below SizeInBytes, so a last record may omit its
 * tail padding, as the runtime's parser allows.  An unknown record type
 * or a record that does not fit is malformed (the runtime returns
 * E_INVALIDARG) and is reported like an unserializable field.
 *
 * Wire form: a uint64 record count, then per record the type enum and
 * the payload under its own codec.  The packed layout never crosses the
 * wire; the decoder rebuilds it in its own ABI and recomputes SizeInBytes.
 */

#ifndef NPT_PROTOCOL_CODEC_D3D12_PIPELINE_STATE_STREAM_DESC_H
#define NPT_PROTOCOL_CODEC_D3D12_PIPELINE_STATE_STREAM_DESC_H

#if defined(__cplusplus)
#define NPT_PSS_ALIGNOF(T) alignof(T)
#else
#define NPT_PSS_ALIGNOF(T) _Alignof(T)
#endif

/* Record types and the payload type documented for each; every payload
 * has a generated codec.  ROOT_SIGNATURE is handled apart: its payload is
 * an ID3D12RootSignature * that travels as a COM handle. */
#define NPT_PSS_RECORDS(X) \
    X(VS, D3D12_SHADER_BYTECODE) \
    X(PS, D3D12_SHADER_BYTECODE) \
    X(DS, D3D12_SHADER_BYTECODE) \
    X(HS, D3D12_SHADER_BYTECODE) \
    X(GS, D3D12_SHADER_BYTECODE) \
    X(CS, D3D12_SHADER_BYTECODE) \
    X(STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC) \
    X(BLEND, D3D12_BLEND_DESC) \
    X(SAMPLE_MASK, UINT) \
    X(RASTERIZER, D3D12_RASTERIZER_DESC) \
    X(DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC) \
    X(INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC) \
    X(IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE) \
    X(PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE) \
    X(RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY) \
    X(DEPTH_STENCIL_FORMAT, DXGI_FORMAT) \
    X(SAMPLE_DESC, DXGI_SAMPLE_DESC) \
    X(NODE_MASK, UINT) \
    X(CACHED_PSO, D3D12_CACHED_PIPELINE_STATE) \
    X(FLAGS, D3D12_PIPELINE_STATE_FLAGS) \
    X(DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1) \
    X(VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC) \
    X(AS, D3D12_SHADER_BYTECODE) \
    X(MS, D3D12_SHADER_BYTECODE) \
    X(DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2) \
    X(RASTERIZER1, D3D12_RASTERIZER_DESC1) \
    X(RASTERIZER2, D3D12_RASTERIZER_DESC2)

/* One record of a stream, as offsets from the stream base. */
struct npt_pss_record {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    size_t payload_off;
    size_t payload_size;
    size_t next_off;      /* where the following record starts */
};

static inline size_t
npt_pss_align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* Payload size and alignment of a record type; 0 for an unknown type. */
static inline size_t
npt_pss_payload(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, size_t *align)
{
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
        *align = NPT_PSS_ALIGNOF(ID3D12RootSignature *);
        return sizeof(ID3D12RootSignature *);
#define NPT_PSS_CASE(tag, T) \
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##tag: \
        *align = NPT_PSS_ALIGNOF(T); \
        return sizeof(T);
    NPT_PSS_RECORDS(NPT_PSS_CASE)
#undef NPT_PSS_CASE
    default:
        *align = 1;
        return 0;
    }
}

/* Lay out a record of `type` whose tag sits at `off`; 0 for an unknown type. */
static inline int
npt_pss_layout(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, size_t off,
               struct npt_pss_record *rec)
{
    size_t align;
    rec->type = type;
    rec->payload_size = npt_pss_payload(type, &align);
    if (!rec->payload_size)
        return 0;
    rec->payload_off = npt_pss_align_up(off + sizeof(type), align);
    rec->next_off = npt_pss_align_up(rec->payload_off + rec->payload_size,
                                     sizeof(void *));
    return 1;
}

/* Read the record at *off and advance.  Returns 1 with the record, 0 at
 * the end of the stream, -1 for a malformed one (unaligned base, unknown
 * type, or a record that does not fit SizeInBytes). */
static inline int
npt_pss_next(const D3D12_PIPELINE_STATE_STREAM_DESC *val, size_t *off,
             struct npt_pss_record *rec)
{
    const uint8_t *base = (const uint8_t *)val->pPipelineStateSubobjectStream;
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    if (!base || *off >= val->SizeInBytes)
        return 0;
    if ((uintptr_t)base % sizeof(void *) ||
        *off + sizeof(type) > val->SizeInBytes)
        return -1;
    memcpy(&type, base + *off, sizeof(type));
    if (!npt_pss_layout(type, *off, rec) ||
        rec->payload_off + rec->payload_size > val->SizeInBytes)
        return -1;
    *off = rec->next_off;
    return 1;
}

/* Largest record; bounds the decoder's allocation. */
static inline size_t
npt_pss_max_record_size(void)
{
    struct npt_pss_record rec;
    size_t max = 0;
    npt_pss_layout(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, 0, &rec);
    max = rec.next_off;
#define NPT_PSS_MAX(tag, T) \
    npt_pss_layout(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##tag, 0, &rec); \
    if (rec.next_off > max) \
        max = rec.next_off;
    NPT_PSS_RECORDS(NPT_PSS_MAX)
#undef NPT_PSS_MAX
    return max;
}

static inline size_t
npt_sizeof_D3D12_PIPELINE_STATE_STREAM_DESC(const D3D12_PIPELINE_STATE_STREAM_DESC *val, int max_mode)
{
    const uint8_t *base = (const uint8_t *)val->pPipelineStateSubobjectStream;
    size_t size = npt_sizeof_array_count(0);
    size_t off = 0;
    struct npt_pss_record rec;
    int r;
    while ((r = npt_pss_next(val, &off, &rec)) > 0) {
        const uint8_t *p = base + rec.payload_off;
        size += npt_sizeof_D3D12_PIPELINE_STATE_SUBOBJECT_TYPE(&rec.type, max_mode);
        switch (rec.type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            size += npt_sizeof_com_handle();
            break;
#define NPT_PSS_CASE(tag, T) \
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##tag: \
            size += npt_sizeof_##T((const T *)p, max_mode); \
            break;
        NPT_PSS_RECORDS(NPT_PSS_CASE)
#undef NPT_PSS_CASE
        default:
            break;
        }
    }
    return r < 0 ? 0 : size;
}

static inline void
npt_encode_D3D12_PIPELINE_STATE_STREAM_DESC(struct npt_cs_encoder *enc, const D3D12_PIPELINE_STATE_STREAM_DESC *val)
{
    const uint8_t *base = (const uint8_t *)val->pPipelineStateSubobjectStream;
    size_t off = 0;
    uint64_t count = 0;
    struct npt_pss_record rec;
    int r;
    /* Count first: a malformed stream sends nothing. */
    while ((r = npt_pss_next(val, &off, &rec)) > 0)
        count++;
    if (r < 0) {
        npt_cs_encoder_set_fatal(enc);
        return;
    }
    npt_encode_array_count(enc, count);
    off = 0;
    while (npt_pss_next(val, &off, &rec) > 0) {
        const uint8_t *p = base + rec.payload_off;
        npt_encode_D3D12_PIPELINE_STATE_SUBOBJECT_TYPE(enc, &rec.type);
        switch (rec.type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
            ID3D12RootSignature *rs;
            memcpy(&rs, p, sizeof(rs));
            npt_encode_com_handle(enc, npt_object_get_id(rs));
            break;
        }
#define NPT_PSS_CASE(tag, T) \
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##tag: \
            npt_encode_##T(enc, (const T *)p); \
            break;
        NPT_PSS_RECORDS(NPT_PSS_CASE)
#undef NPT_PSS_CASE
        default:
            break;
        }
    }
}

static inline void
npt_decode_D3D12_PIPELINE_STATE_STREAM_DESC(struct npt_cs_decoder *dec, D3D12_PIPELINE_STATE_STREAM_DESC *val)
{
    const uint64_t count = npt_decode_array_count_unchecked(dec);
    uint64_t i;
    uint8_t *base;
    size_t off = 0;
    val->SizeInBytes = 0;
    val->pPipelineStateSubobjectStream = NULL;
    if (!count)
        return;
    base = (uint8_t *)npt_cs_decoder_alloc_temp_array(dec, npt_pss_max_record_size(), count);
    if (!base)
        return;
    for (i = 0; i < count; i++) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        struct npt_pss_record rec;
        uint8_t *p;
        npt_decode_D3D12_PIPELINE_STATE_SUBOBJECT_TYPE(dec, &type);
        if (!npt_pss_layout(type, off, &rec)) {
            npt_cs_decoder_set_fatal(dec);
            return;
        }
        memcpy(base + off, &type, sizeof(type));
        p = base + rec.payload_off;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
            npt_object_id id;
            ID3D12RootSignature *rs;
            npt_decode_com_handle(dec, &id);
            rs = (ID3D12RootSignature *)npt_object_from_id(id);
            memcpy(p, &rs, sizeof(rs));
            break;
        }
#define NPT_PSS_CASE(tag, T) \
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##tag: \
            npt_decode_##T(dec, (T *)p); \
            break;
        NPT_PSS_RECORDS(NPT_PSS_CASE)
#undef NPT_PSS_CASE
        default:
            break;
        }
        off = rec.next_off;
    }
    val->SizeInBytes = off;
    val->pPipelineStateSubobjectStream = base;
}

#if NPT_CODEC_IS_HOST
/* Resolve the guest object id the decoder left in each root-signature
 * record (npt_object_from_id is the identity on the host). */
static inline void
npt_replace_D3D12_PIPELINE_STATE_STREAM_DESC_handle(struct npt_dispatch_context *ctx, D3D12_PIPELINE_STATE_STREAM_DESC *val)
{
    uint8_t *base = (uint8_t *)val->pPipelineStateSubobjectStream;
    size_t off = 0;
    struct npt_pss_record rec;
    while (npt_pss_next(val, &off, &rec) > 0) {
        if (rec.type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
            uint8_t *p = base + rec.payload_off;
            ID3D12RootSignature *rs;
            memcpy(&rs, p, sizeof(rs));
            rs = (ID3D12RootSignature *)npt_cs_handle_lookup(
                ctx, (npt_object_id)(uintptr_t)rs, NPT_OBJECT_TYPE_ID3D12ROOTSIGNATURE);
            memcpy(p, &rs, sizeof(rs));
        }
    }
}
#endif /* NPT_CODEC_IS_HOST */

#undef NPT_PSS_ALIGNOF

#endif /* NPT_PROTOCOL_CODEC_D3D12_PIPELINE_STATE_STREAM_DESC_H */
