// The slot map (engine slot value -> Metal object), tokens, and the engine's format encodings.
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "rn.h"

// ---------------- slot map ----------------
#define MAPN (1u << 17)
static struct { uint32_t key; RnObj o; } *map;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t hslot(uint32_t k) { return (k * 2654435761u) & (MAPN - 1); }
RnObj *rn_get(uint32_t k) {
    if (!k || !map) return NULL;
    for (uint32_t i = hslot(k), n = 0; n < MAPN; i = (i + 1) & (MAPN - 1), n++) {
        if (map[i].key == k) return map[i].o.kind ? &map[i].o : NULL;
        if (!map[i].key) return NULL;
    }
    return NULL;
}
RnObj *rn_put(uint32_t k, uint8_t kind) {
    pthread_mutex_lock(&mu);
    if (!map) map = calloc(MAPN, sizeof *map);
    uint32_t i = hslot(k), tomb = ~0u;
    for (uint32_t n = 0; n < MAPN; i = (i + 1) & (MAPN - 1), n++) {
        if (map[i].key == k) break;
        if (map[i].key == 0) { if (tomb != ~0u) i = tomb; break; }
        if (!map[i].o.kind && tomb == ~0u) tomb = i;
    }
    if (map[i].key != k) { map[i].key = k; memset(&map[i].o, 0, sizeof map[i].o); }
    map[i].o.kind = kind;
    pthread_mutex_unlock(&mu);
    return &map[i].o;
}
void rn_del(uint32_t k) { RnObj *o = rn_get(k); if (o) o->kind = 0; }   // the key stays as a tombstone

void rn_mark_readback(MtlTex t) {
    if (!map || !t) return;
    for (uint32_t i = 0; i < MAPN; i++) if (map[i].o.kind == RN_TEX && map[i].o.tex == t) map[i].o.readback = 1;
}
int rn_native(void) { return !mtlr_shadow(); }
uint32_t rn_token(void) {
    static uint32_t base, used;
    pthread_mutex_lock(&mu);
    if (!base || used >= 0x10000) { base = gvm_alloc(0, 0x10000, 1, 4); used = 0; }
    uint32_t t = base + used; used += 16;
    pthread_mutex_unlock(&mu);
    return t;
}
uint32_t rn_galloc(uint32_t size) { return gvm_alloc(0, size ? size : 1, 1, 4); }

// ---------------- formats (the engine stores D3DFMT codes) ----------------
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
uint32_t rn_pixel_format(uint32_t f, uint8_t *swz) {   // D3DFMT (the engine's encoding) -> MTLPixelFormat
    *swz = MTLR_SWZ_RGBA;
    switch (f) {
    case 21: return MTLR_PF_BGRA8;                              // A8R8G8B8
    case 22: *swz = MTLR_SWZ_RGB1; return MTLR_PF_BGRA8;        // X8R8G8B8
    case 23: return MTLR_PF_B5G6R5;
    case 25: return MTLR_PF_BGR5A1;                             // A1R5G5B5
    case 28: return MTLR_PF_A8;
    case 31: return MTLR_PF_RGB10A2;                            // A2B10G10R10
    case 32: return MTLR_PF_RGBA8;                              // A8B8G8R8
    case 33: *swz = MTLR_SWZ_RGB1; return MTLR_PF_RGBA8;        // X8B8G8R8
    case 34: return MTLR_PF_RG16;                               // G16R16
    case 36: return MTLR_PF_RGBA16;
    case 50: *swz = MTLR_SWZ_RRR1; return MTLR_PF_R8;           // L8
    case 51: *swz = MTLR_SWZ_RRRG; return MTLR_PF_RG8;          // A8L8
    case 81: *swz = MTLR_SWZ_RRR1; return MTLR_PF_R16;          // L16
    case 111: return MTLR_PF_R16F;
    case 112: return MTLR_PF_RG16F;
    case 113: return MTLR_PF_RGBA16F;
    case 114: return MTLR_PF_R32F;
    case 115: return MTLR_PF_RG32F;
    case 116: return MTLR_PF_RGBA32F;
    case FOURCC('D','X','T','1'): return MTLR_PF_BC1;
    case FOURCC('D','X','T','2'): case FOURCC('D','X','T','3'): return MTLR_PF_BC2;
    case FOURCC('D','X','T','4'): case FOURCC('D','X','T','5'): return MTLR_PF_BC3;
    case FOURCC('A','T','I','1'): return MTLR_PF_BC4;
    case FOURCC('A','T','I','2'): return MTLR_PF_BC5;
    case 75: case FOURCC('I','N','T','Z'): return MTLR_PF_D32F_S8;  // D24S8
    case 77: case 80: case 70: case 71: case FOURCC('D','F','2','4'): case FOURCC('D','F','1','6'): return MTLR_PF_D32F;
    }
    return 0;
}

uint8_t rn_vertex_format(uint8_t t) {   // D3DDECLTYPE (the engine's encoding) -> MTLVertexFormat
    static const uint8_t m[17] = {MTLR_VF_FLOAT, MTLR_VF_FLOAT2, MTLR_VF_FLOAT3, MTLR_VF_FLOAT4, MTLR_VF_UCHAR4N_BGRA,
        MTLR_VF_UCHAR4, MTLR_VF_SHORT2, MTLR_VF_SHORT4, MTLR_VF_UCHAR4N, MTLR_VF_SHORT2N, MTLR_VF_SHORT4N, MTLR_VF_USHORT2N,
        MTLR_VF_USHORT4N, MTLR_VF_UINT1010102N, MTLR_VF_INT1010102N, MTLR_VF_HALF2, MTLR_VF_HALF4};
    return t < 17 ? m[t] : MTLR_VF_FLOAT4;
}
