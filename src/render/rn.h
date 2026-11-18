// RE1 HD native Metal renderer: the engine's D3D9 call sites rewritten (rn_sites.c,
// patches.txt) and its draw packet replay replaced (rn_draw.c). The engine keeps D3D object pointers in its resource
// objects (texture +0x34, surface +0x14, buffer +0x20, shaders, declarations); with the native renderer those hold
// tokens, in shadow mode (RT_METAL_SHADOW=1) the real D3D objects. Either way: slot value -> RnObj.
#pragma once
#include "rt32.h"
#include "mtl_render.h"

enum { RN_TEX = 1, RN_SURF, RN_BUF, RN_VS, RN_PS, RN_DECL };
typedef struct {
    uint8_t kind;
    uint64_t used_seq;          // RN_BUF: command buffer that last read it (mtlr_seq_now)
    uint8_t owns;               // RN_SURF: its own texture (CreateRenderTarget / CreateDepthStencilSurface), freed with it
    uint32_t used_frame;        // owned surfaces: last frame a draw used it (rn_frame), released: the game let it go
    uint8_t released;
    MtlTex tex;                 // RN_TEX: the texture; RN_SURF: the texture it is a level/face of (or its own)
    uint8_t face, level;        // RN_SURF
    uint32_t hash;              // RN_VS / RN_PS: bytecode hash (sm3msl)
    uint32_t layout;            // RN_DECL: mtlr_vertex_layout id
    uint32_t addr, size;        // RN_BUF: current guest address of the data (renamed on discard), size
    uint8_t dynamic, index32;
    uint32_t blk[8], blk_retired[8];   // RN_BUF dynamic: data blocks (discard renames to one the GPU is done with)
    uint8_t nblk, cur;
    uint32_t lock_ptr, lock_pitch, lock_slice, lock_index;   // texture locks: staging block, pitch, its size, face << 8 | level
    uint32_t lock_data;         // the pending lock's data (uploaded / mirrored at unlock)
    uint32_t lock_box_slice;    // volume locks: slice pitch
    int lock_rect[4];           // x, y, w, h of the locked region (w = 0: the whole level)
    uint8_t backbuffer;
    uint8_t readback;           // RN_TEX: GetRenderTargetData wrote it: a lock downloads the contents
} RnObj;

RnObj *rn_get(uint32_t slot);               // NULL if unknown
RnObj *rn_put(uint32_t slot, uint8_t kind); // new or existing entry for that slot value
void rn_del(uint32_t slot);
uint32_t rn_token(void);                    // a fresh guest token (native renderer mode)
int rn_native(void);                        // the engine's D3D9 code no longer runs (not shadow mode)
void rn_setup(void);                        // load the shaders, open the output (first use)

#define SRENDER (rd32(0xd5a048u))           // sRender::mpInstance
uint32_t rn_galloc(uint32_t size);          // guest memory for buffer data (64 KiB aligned regions, never freed yet)
uint32_t rn_pixel_format(uint32_t d3dfmt, uint8_t *swizzle);
uint8_t rn_vertex_format(uint8_t d3ddecltype);
void rn_mark_readback(MtlTex t);             // every texture object with this Metal texture downloads on lock
extern uint32_t rn_frame;   // frames presented (rn_sites.c)
