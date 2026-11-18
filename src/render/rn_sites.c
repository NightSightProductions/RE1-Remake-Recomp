#include <time.h>
// The engine's D3D9 call sites, rewritten (patches.txt: "ADDR @call N_s_*"). Each native does that call's job for
// the engine on Metal and pops the call's arguments (ARG(0) is the object the engine called through).
//   native mode: no D3D9 exists; the engine receives tokens (guest objects whose every vtable slot traps, so a call
//                site that was not rewritten fails loudly with its address)
//   shadow mode (RT_METAL_SHADOW=1): the original D3D9 call runs too (DXVK), and the Metal side mirrors it, keyed by
//                the real D3D object
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include "rn.h"

void call_ind(CPU *c, uint32_t t);
void rt_cocoa_screen_size(int *w, int *h);
int rt_cocoa_refresh_hz(void);
int rt_cocoa_set_fullscreen(uint32_t hwnd, int on);
void I_d3d9_Direct3DCreate9(CPU *c);

// shadow mode: make the original call through the object's vtable (the callee pops the return address and the
// arguments; the patch pops a return address after us, so leave one slot)
static void forward(CPU *c, uint32_t off) {
    call_ind(c, rd32(rd32(ARG(0)) + off));
    c->esp -= 4;
}
#define SHADOW mtlr_shadow()
static uint32_t present_count;    // frames presented (frame_end): buffer renaming
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define RETURN(v, nargs) do { c->eax = (v); NATIVE_POP(nargs); return; } while (0)

// ---------------- tokens ----------------
enum { TK_D3D = 1, TK_DEVICE, TK_TEX, TK_CUBE, TK_SURF, TK_VB, TK_IB, TK_VS, TK_PS, TK_DECL, TK_QUERY, TK_N };
static const char *tk_name[TK_N] = {"?", "IDirect3D9", "IDirect3DDevice9", "IDirect3DTexture9", "IDirect3DCubeTexture9",
    "IDirect3DSurface9", "IDirect3DVertexBuffer9", "IDirect3DIndexBuffer9", "IDirect3DVertexShader9", "IDirect3DPixelShader9",
    "IDirect3DVertexDeclaration9", "IDirect3DQuery9"};
#include "rn_d3d9_slots.h"
static const RnSlot *tk_slots[TK_N] = {NULL, rn_slots_d3d9_m, rn_slots_device_m, rn_slots_texture_m, rn_slots_cube_m, rn_slots_surface_m,
    rn_slots_vbuffer_m, rn_slots_ibuffer_m, rn_slots_shader_m, rn_slots_shader_m, rn_slots_decl_m, rn_slots_query_m};
static const int tk_nslots[TK_N] = {0, RN_NSLOTS_d3d9_m, RN_NSLOTS_device_m, RN_NSLOTS_texture_m, RN_NSLOTS_cube_m, RN_NSLOTS_surface_m,
    RN_NSLOTS_vbuffer_m, RN_NSLOTS_ibuffer_m, RN_NSLOTS_shader_m, RN_NSLOTS_shader_m, RN_NSLOTS_decl_m, RN_NSLOTS_query_m};
// a call through a token that no rewritten site handled: log it once per call site (with the address to add to
// patches.txt), pop its arguments and report success, so one run finds every missing site (RT_RN_STRICT=1: stop)
static void trap(CPU *c, uintptr_t ctx) {
    int kind = (int)(ctx >> 16); unsigned slot = (unsigned)(ctx & 0xFFFF);
    const char *name = slot < (unsigned)tk_nslots[kind] ? tk_slots[kind][slot].name : "?";
    int nargs = slot < (unsigned)tk_nslots[kind] ? tk_slots[kind][slot].nargs : 1;
    uint32_t site = rd32(c->esp);
    static uint32_t seen[512]; static int nseen; int known = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == site) known = 1;
    if (!known) {
        if (nseen < 512) seen[nseen++] = site;
        // the call's address from its encoding: call [reg+disp32] (FF /2, mod 10) is 6 bytes, [reg+disp8] 3, reg 2
        uint32_t at = rd8(site - 6) == 0xFF && (rd8(site - 5) & 0xF8) >= 0x90 && (rd8(site - 5) & 0xF8) <= 0x97 ? site - 6
                    : rd8(site - 3) == 0xFF && (rd8(site - 2) & 0xF8) >= 0x50 && (rd8(site - 2) & 0xF8) <= 0x57 ? site - 3 : site - 2;
        rt_log("[rn] D3D9 call site not rewritten: %s::%s (slot %u) returning to %08x - add %08x to patches.txt",
               tk_name[kind], name, slot, site, at);
        if (getenv("RT_RN_STRICT")) rt_fatal("[rn] RT_RN_STRICT: stopping at the first unrewritten D3D9 call");
    }
    c->eax = !strcmp(name, "AddRef") ? 1 : 0;
    c->esp += 4u * (uint32_t)nargs;
}
static uint32_t trap_vtable(int kind) {
    static uint32_t vt[TK_N];
    if (!vt[kind]) {
        uint32_t t = gcalloc(128, 4);
        for (uint32_t i = 0; i < 128; i++) wr32(t + 4 * i, rt_host_fn_ctx(trap, ((uintptr_t)kind << 16) | i, "rn_trap"));
        vt[kind] = t;
    }
    return vt[kind];
}
static uint32_t token(int kind) {
    uint32_t t = rn_token();
    wr32(t, trap_vtable(kind)); wr32(t + 4, 0x4B544E52u); wr32(t + 8, (uint32_t)kind);
    return t;
}
// the object a creation call hands back: the real one (shadow, after forwarding) or a fresh token
static uint32_t created(CPU *c, uint32_t out, int kind) { (void)c; if (SHADOW) return out ? rd32(out) : 0; uint32_t t = token(kind); if (out) wr32(out, t); return t; }

// ---------------- formats ----------------
uint32_t rn_pixel_format(uint32_t d3dfmt, uint8_t *swz);   // rn_res.c

// ---------------- init: the adapter ----------------
// Direct3DCreate9(version) (the import, called at 7bdce8)
void N_s_create_d3d(CPU *c) {
    rn_setup();
    if (SHADOW) { I_d3d9_Direct3DCreate9(c); return; }
    RETURN(token(TK_D3D), 1);
}
void N_s_perf_zero(CPU *c) { c->eax = 0; }        // D3DPERF_SetOptions(1 arg) / GetStatus(0): the stdcall pops are done per site
void N_s_perf_set_options(CPU *c) { RETURN(0, 1); }
// GetAdapterIdentifier(this, adapter, flags, D3DADAPTER_IDENTIFIER9 *)
void N_s_adapter_identifier(CPU *c) {
    if (SHADOW) { forward(c, 0x14); return; }
    uint32_t id = ARG(3);
    memset(G2H(id), 0, 0x448);
    strcpy((char *)G2H(id), "metal"); strcpy((char *)G2H(id + 0x200), "Apple GPU (Metal)"); strcpy((char *)G2H(id + 0x400), "metal");
    wr32(id + 0x418, 0x106B);   // VendorId: Apple
    RETURN(0, 4);
}
// GetDeviceCaps(this, adapter, type, D3DCAPS9 *): the engine reads the shader versions, DevCaps and TextureCaps
void N_s_device_caps(CPU *c) {
    if (SHADOW) { forward(c, 0x38); return; }
    uint32_t p = ARG(3);
    memset(G2H(p), 0, 0x130);
    wr32(p + 0x1c, 0x001BBEF0);           // DevCaps: hardware T&L, pure device, ...
    wr32(p + 0x3c, 0x0001EC45);           // TextureCaps: no POW2 restriction, cube maps, volume maps, mip maps
    wr32(p + 0x58, 8192); wr32(p + 0x5c, 8192); wr32(p + 0x60, 2048);   // max texture width / height / volume extent
    wr32(p + 0xc4, 0xFFFE0300u);          // VertexShaderVersion 3.0
    wr32(p + 0xc8, 256);                  // MaxVertexShaderConst
    wr32(p + 0xcc, 0xFFFF0300u);          // PixelShaderVersion 3.0
    RETURN(0, 4);
}
// CheckDeviceFormat(this, adapter, type, adapter fmt, usage, resource type, fmt): everything but R2VB
void N_s_check_format(CPU *c) {
    if (SHADOW) { forward(c, 0x28); return; }
    RETURN(ARG(6) == 0x42563252u ? 0x8876086Au : 0, 7);
}
// CheckDeviceMultiSampleType(this, adapter, type, fmt, windowed, samples, *quality): 2x and 4x MSAA
void N_s_check_msaa(CPU *c) {
    if (SHADOW) { forward(c, 0x2c); return; }
    uint32_t n = ARG(5), q = ARG(6);
    if (q) wr32(q, 1);
    RETURN(n <= 1 || n == 2 || n == 4 ? 0 : 0x8876086Au, 7);
}
// the display modes offered: what the connected displays support (points and Retina pixels) plus standard 16:9 sizes
int rt_cocoa_display_modes(int *w, int *h, int max);
static int modes(uint32_t *w, uint32_t *h) {
    static int n, mw[96], mh[96];
    if (!n) {
        // the original tops out at 2560x1600 (16:10 modes are drawn 16:9 inside, the prerendered backgrounds being
        // 1920x1440) and offers no 16:9 mode above 1080p: keep that list
        int aw[96], ah[96], k = rt_cocoa_display_modes(aw, ah, 96);
        for (int i = 0; i < k; i++) {
            if (aw[i] > 2560 || ah[i] > 1600 || (ah[i] > 1080 && aw[i] * 9 == ah[i] * 16)) continue;
            mw[n] = aw[i]; mh[n] = ah[i]; n++;
        }
    }
    if (!n) { mw[0] = 1280; mh[0] = 720; n = 1; }
    if (w) for (int i = 0; i < n; i++) { w[i] = (uint32_t)mw[i]; h[i] = (uint32_t)mh[i]; }
    return n;
}
void N_s_mode_count(CPU *c) {   // GetAdapterModeCount(this, adapter, fmt)
    if (SHADOW) { forward(c, 0x18); return; }
    RETURN((uint32_t)modes(NULL, NULL), 3);
}
void N_s_enum_mode(CPU *c) {    // EnumAdapterModes(this, adapter, fmt, index, D3DDISPLAYMODE *)
    if (SHADOW) { forward(c, 0x1c); return; }
    uint32_t w[96], h[96]; int n = modes(w, h), i = (int)ARG(3), p = (int)ARG(4);
    if (i >= n) RETURN(0x8876086Cu, 5);
    wr32((uint32_t)p, w[i]); wr32((uint32_t)p + 4, h[i]); wr32((uint32_t)p + 8, (uint32_t)rt_cocoa_refresh_hz()); wr32((uint32_t)p + 12, 0x16);
    RETURN(0, 5);
}
void N_s_display_mode(CPU *c) { // GetAdapterDisplayMode(this, adapter, D3DDISPLAYMODE *)
    if (SHADOW) { forward(c, 0x20); return; }
    void rt_cocoa_screen_pixels(int *w, int *h);
    int w, h; rt_cocoa_screen_pixels(&w, &h);   // pixels: the engine offers no mode above the desktop
    uint32_t p = ARG(2);
    wr32(p, (uint32_t)w); wr32(p + 4, (uint32_t)h); wr32(p + 8, (uint32_t)rt_cocoa_refresh_hz()); wr32(p + 12, 0x16);
    RETURN(0, 3);
}

// ---------------- the device ----------------
// D3DPRESENT_PARAMETERS: +0 width, +4 height, +0x1c window, +0x34 presentation interval
static int vsync = 1;
static void output_from(uint32_t pp, uint32_t focus) {
    static uint32_t last;   // Reset passes no window: the device keeps its own
    uint32_t hwnd = rd32(pp + 0x1c) ? rd32(pp + 0x1c) : focus ? focus : last;
    last = hwnd;
    // D3DPRESENT_PARAMETERS +0x20 Windowed: FALSE asks for exclusive fullscreen -> the macOS fullscreen space (the
    // layer scales the back buffer to the screen); TRUE leaves it
    if (!SHADOW && hwnd) {
        void rt_cocoa_set_cursor_fullscreen(int on);
        rt_cocoa_set_fullscreen(hwnd, rd32(pp + 0x20) == 0);
        rt_cocoa_set_cursor_fullscreen(rd32(pp + 0x20) == 0);
    }
    vsync = rd32(pp + 0x34) != 0x80000000u;   // D3DPRESENT_INTERVAL_IMMEDIATE
    mtlr_set_window(hwnd, (int)rd32(pp), (int)rd32(pp + 4));
}
// CreateDevice(this, adapter, type, focus window, flags, D3DPRESENT_PARAMETERS *, **device)
void N_s_create_device(CPU *c) {
    rn_setup();
    uint32_t focus = ARG(3), pp = ARG(5), out = ARG(6);
    if (SHADOW) { forward(c, 0x40); output_from(pp, focus); return; }
    output_from(pp, focus);
    created(c, out, TK_DEVICE);
    RETURN(0, 7);
}
void N_s_reset(CPU *c) {        // Reset(this, D3DPRESENT_PARAMETERS *)
    uint32_t pp = ARG(1);
    if (SHADOW) { forward(c, 0x40); output_from(pp, 0); return; }
    output_from(pp, 0);
    RETURN(0, 2);
}
void N_s_test_coop(CPU *c) { if (SHADOW) { forward(c, 0xc); return; } RETURN(0, 1); }
void N_s_texture_mem(CPU *c) { if (SHADOW) { forward(c, 0x10); return; } RETURN(0x7FC00000u, 1); }
void N_s_evict(CPU *c) { if (SHADOW) { forward(c, 0x14); return; } RETURN(0, 1); }
void N_s_cursor_pos(CPU *c) { if (SHADOW) { forward(c, 0x2c); return; } RETURN(0, 4); }   // SetCursorPosition(this, x, y, flags)
// SetGammaRamp(this, swap chain, flags, D3DGAMMARAMP * {red[256], green[256], blue[256]}): the brightness setting
void N_s_gamma(CPU *c) {
    uint32_t r = ARG(3);
    if (r) mtlr_set_gamma((const uint16_t *)G2H(r));
    if (SHADOW) { forward(c, 0x54); return; }
    RETURN(0, 4);
}
// GetBackBuffer(this, swap chain, index, type, **surface)
void N_s_back_buffer(CPU *c) {
    uint32_t out = ARG(4);              // (arguments are read before forwarding: forward() moves esp)
    if (SHADOW) { forward(c, 0x48); }
    uint32_t s = created(c, out, TK_SURF);
    if (s) { RnObj *o = rn_put(s, RN_SURF); o->tex = mtlr_backbuffer(); o->face = o->level = 0; o->backbuffer = 1; }
    if (!SHADOW) RETURN(0, 5);
}
// IUnknown on surfaces/textures the engine releases or adds references to
// Surfaces that own their texture (CreateRenderTarget, CreateDepthStencilSurface): the engine releases some it goes on
// drawing into (it leans on references D3D9 keeps that it never takes itself), so a release only marks it; the texture
// is freed once no draw has used it for 300 frames (rn_sweep_surfaces, each frame)
uint32_t rn_frame;
static uint32_t released[512]; static int nreleased;
static void surf_release(uint32_t s, RnObj *r) {
    if (r->released) return;
    r->released = 1;
    if (nreleased < 512) released[nreleased++] = s;
}
static void rn_sweep_surfaces(void) {
    for (int i = 0; i < nreleased; ) {
        RnObj *r = rn_get(released[i]);
        if (r && r->kind == RN_SURF && r->owns && r->released && rn_frame - r->used_frame < 300) { i++; continue; }
        if (r && r->kind == RN_SURF && r->owns && r->released) { if (!SHADOW) mtlr_tex_destroy(r->tex); rn_del(released[i]); }
        released[i] = released[--nreleased];
    }
}
void N_s_addref(CPU *c) {
    if (SHADOW) { forward(c, 4); return; }
    RnObj *r = rn_get(ARG(0));
    if (r && r->kind == RN_SURF && r->owns) r->released = 0;   // the game holds it again
    RETURN(1, 1);
}
// SetDepthStencilSurface(this, surface) outside the packet replay (shutdown): targets are set by the replay itself
void N_s_set_depth_ignored(CPU *c) { if (SHADOW) { forward(c, 0x9c); return; } RETURN(0, 2); }
void N_s_release(CPU *c) {
    uint32_t o = ARG(0);
    RnObj *r = rn_get(o);
    if (r && r->kind == RN_SURF && r->owns) { surf_release(o, r); if (SHADOW) { forward(c, 8); return; } RETURN(0, 1); }
    // textures (a texture's level surfaces own nothing: the texture frees it)
    if (r && r->kind == RN_TEX && !SHADOW) mtlr_tex_destroy(r->tex);
    if (r && r->kind != RN_SURF) rn_del(o);
    if (SHADOW) { forward(c, 8); return; }
    RETURN(0, 1);
}

// ---------------- frames ----------------
extern int rn_stat[8], rn_trace;
static void frame_end(void) {
    static int frames;
    if (getenv("RT_METAL_TEST")) {   // backend self-test: magenta back buffer
        MtlTarget t[4] = {{mtlr_backbuffer(), 0, 0, 0}}; MtlTarget d = {0};
        const MtlTexDesc *bd = mtlr_tex_desc(mtlr_backbuffer());
        mtlr_set_targets(t, d); if (bd) mtlr_set_viewport(0, 0, (float)bd->width, (float)bd->height, 0, 1);
        float m[4] = {1, 0, 1, 1}; mtlr_clear(MTLR_CLEAR_COLOR, m, 1, 0);
    }
    mtlr_present(vsync);
    present_count++;
    rn_frame++;
    rn_sweep_surfaces();
    static int fps_on = -1; static double last, sum, worst; static int over20, over40, nft;
    if (fps_on < 0) fps_on = getenv("RT_RN_FPS") != NULL;
    if (fps_on) {   // RT_RN_FPS=1: frame times (present to present) every 120 frames
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6, dt = last ? now - last : 0; last = now;
        sum += dt; if (dt > worst) worst = dt; over20 += dt > 20; over40 += dt > 40;
        extern double rn_replay_ms; double pso_ms, dr_ms; int pso_n; mtlr_frame_stats(&pso_ms, &pso_n, &dr_ms);
        if (dt > 25) rt_log("[rn] hitch %.1f ms: replay %.1f ms, %d new pipelines %.1f ms, drawable wait %.1f ms",
                            dt, rn_replay_ms, pso_n, pso_ms, dr_ms);
        rn_replay_ms = 0;
        if (++nft == 120) {
            extern double rn_wait_ms; static double gsum, wsum_last;
            gsum += mtlr_gpu_ms(); (void)wsum_last;
            rt_log("[rn] fps %.1f avg %.2f ms max %.2f ms >20ms %d >40ms %d | gpu %.2f ms/frame, waiting for gpu %.2f ms/frame",
                   1000 * nft / sum, sum / nft, worst, over20, over40, gsum / nft, rn_wait_ms / nft);
            gsum = 0; rn_wait_ms = 0;
            nft = 0; sum = worst = 0; over20 = over40 = 0;
        }
    }
    if (++frames % 120 == 0)
        rt_log("[rn] frame %d: packets %d draws %d indexed %d clears %d copies %d skipped %d (no shader obj %d) instanced %d",
               frames, rn_stat[7], rn_stat[0], rn_stat[1], rn_stat[2], rn_stat[3], rn_stat[4], rn_stat[5], rn_stat[6]);
    memset(rn_stat, 0, sizeof rn_stat);
    static int dump_at, dump_init, trace_at = -1;
    if (!dump_init) { dump_init = 1; const char *e = getenv("RT_METAL_DUMP"); dump_at = e ? atoi(e) : 0; }
    if (trace_at < 0) { const char *e = getenv("RT_RN_TRACE"); trace_at = e ? atoi(e) : 0; }
    rn_trace = trace_at && frames + 1 == trace_at;
    // RT_METAL_DUMP=N: dump at frame N; negative: every -N frames into metal_dump<tag>/f<frame>
    if (dump_at && (dump_at > 0 ? frames == dump_at : frames % -dump_at == 0)) {
        char d[1100]; snprintf(d, sizeof d, "%s/metal_dump%s", rt_data_dir, getenv("RT_METAL_DUMP_TAG") ? getenv("RT_METAL_DUMP_TAG") : "");
        mkdir(d, 0755);
        if (dump_at < 0) { size_t l = strlen(d); snprintf(d + l, sizeof d - l, "/f%05d", frames); mkdir(d, 0755); }
        mtlr_debug_dump(d);
    }
}
void N_s_present(CPU *c) {      // Present(this, src rect, dst rect, window, dirty region)
    if (SHADOW) forward(c, 0x44);
    frame_end();
    if (!SHADOW) RETURN(0, 5);
}
// Clear(this, count, rects, flags, color, z, stencil): F_7c17d0 clears the back buffer before presenting it
void N_s_clear(CPU *c) {
    uint32_t fl = ARG(3), col = ARG(4), sten = ARG(6);
    float z; uint32_t zu = ARG(5); memcpy(&z, &zu, 4);
    if (SHADOW) { forward(c, 0xac); }
    MtlTarget t[4] = {{mtlr_backbuffer(), 0, 0, 0}}; MtlTarget d = {0};
    const MtlTexDesc *bd = mtlr_tex_desc(mtlr_backbuffer());
    if (bd) {
        mtlr_set_targets(t, d); mtlr_set_viewport(0, 0, (float)bd->width, (float)bd->height, 0, 1);
        float cf[4] = {((col >> 16) & 0xFF) / 255.f, ((col >> 8) & 0xFF) / 255.f, (col & 0xFF) / 255.f, (col >> 24) / 255.f};
        if (fl & 1) mtlr_clear(MTLR_CLEAR_COLOR, cf, z, (uint8_t)sten);
    }
    if (!SHADOW) RETURN(0, 7);
}
// F_7bd640's wait for the GPU: CreateQuery / Issue / GetData / Release.
// The engine reuses buffers and textures after this wait without discarding them, so the wait must be real: Issue
// submits the work so far and keeps a fence, GetData reports done (S_OK) once the GPU has finished it, S_FALSE before.
void N_s_create_query(CPU *c) { if (SHADOW) { forward(c, 0x1d8); return; } created(c, ARG(2), TK_QUERY); RETURN(0, 3); }
// F_7bd640 issues an event query after each frame and waits for it before the next. Buffer locks wait for the GPU
// themselves (N_s_buf_lock), so the query reports the frame before: the GPU keeps one frame in flight and the CPU and
// GPU overlap instead of taking turns.
static uint64_t query_fence, query_prev;
void N_s_query_issue(CPU *c) { if (SHADOW) { forward(c, 0x18); return; } query_prev = query_fence; query_fence = mtlr_fence(); RETURN(0, 2); }
double rn_wait_ms;   // RT_RN_FPS: time the engine spent waiting for the GPU (query issue to done)
static double query_t0; static int query_waited;
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
void N_s_query_data(CPU *c) {
    if (SHADOW) { forward(c, 0x1c); return; }
    int done = mtlr_fence_done(query_prev);
    if (!query_waited) { query_waited = 1; query_t0 = now_ms(); }
    if (done) { rn_wait_ms += now_ms() - query_t0; query_waited = 0; }
    RETURN(done ? 0 : 1, 4);
}

// ---------------- textures ----------------
static void create_log(const char *what, uint32_t w, uint32_t h) { if (getenv("RT_RN_CREATES")) rt_log("[rn] create %s %ux%u", what, w, h); }
static MtlTex make_tex(int type, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt) {
    create_log(type == MTLR_TEX_CUBE ? "cube" : type == MTLR_TEX_3D ? "volume" : "texture", w, h);
    MtlTexDesc d; memset(&d, 0, sizeof d);
    d.type = (uint8_t)type; d.width = w; d.height = type == MTLR_TEX_CUBE ? w : h; d.depth = 1; d.samples = 1;
    d.format = rn_pixel_format(fmt, &d.swizzle);
    if (!levels) { levels = 1; for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) levels++; }
    d.levels = (uint8_t)levels;
    d.usage = MTLR_USE_SAMPLE | ((usage & 3) ? MTLR_USE_TARGET : 0);   // D3DUSAGE_RENDERTARGET / DEPTHSTENCIL
    if (!d.format) { rt_log("[rn] texture %ux%u: unknown format 0x%x", w, h, fmt); return 0; }
    if (d.format == MTLR_PF_D32F_S8 || d.format == MTLR_PF_D32F) d.usage |= MTLR_USE_TARGET;
    return mtlr_tex_create(&d);
}
// CreateTexture(this, w, h, levels, usage, fmt, pool, **tex, shared)
void N_s_create_texture(CPU *c) {
    uint32_t w = ARG(1), h = ARG(2), lv = ARG(3), us = ARG(4), fmt = ARG(5), out = ARG(7);
    if (SHADOW) forward(c, 0x5c);
    uint32_t t = created(c, out, TK_TEX);
    if (t) { RnObj *o = rn_put(t, RN_TEX); o->tex = make_tex(MTLR_TEX_2D, w, h, lv, us, fmt); }
    if (!SHADOW) RETURN(0, 9);
}
// CreateCubeTexture(this, edge, levels, usage, fmt, pool, **tex, shared)
void N_s_create_cube(CPU *c) {
    uint32_t e = ARG(1), lv = ARG(2), us = ARG(3), fmt = ARG(4), out = ARG(6);
    if (SHADOW) forward(c, 0x64);
    uint32_t t = created(c, out, TK_CUBE);
    if (t) { RnObj *o = rn_put(t, RN_TEX); o->tex = make_tex(MTLR_TEX_CUBE, e, e, lv, us, fmt); }
    if (!SHADOW) RETURN(0, 8);
}
// locks: the engine writes into guest memory; the unlock uploads it. Native mode hands out a staging block per lock.
static void lock_begin(RnObj *o, int face, int level, uint32_t lr, uint32_t rect) {
    uint32_t pitch, rows, size = mtlr_tex_level_size(o->tex, level, &pitch, &rows);
    o->lock_rect[0] = o->lock_rect[1] = o->lock_rect[2] = o->lock_rect[3] = 0;
    if (rect) { o->lock_rect[0] = (int)rd32(rect); o->lock_rect[1] = (int)rd32(rect + 4);
                o->lock_rect[2] = (int)rd32(rect + 8) - o->lock_rect[0]; o->lock_rect[3] = (int)rd32(rect + 12) - o->lock_rect[1]; }
    if (!SHADOW) {   // a staging copy of the whole level; a rect lock points into it
        if (o->lock_ptr == 0 || o->lock_slice < size) { o->lock_ptr = rn_galloc(size); o->lock_slice = size; }
        int block, bpb = mtlr_tex_texel_bytes(o->tex, &block);
        uint32_t at = o->lock_ptr + (uint32_t)(o->lock_rect[1] / block) * pitch + (uint32_t)(o->lock_rect[0] / block * bpb);
        wr32(lr, pitch); wr32(lr + 4, at);
        if (o->readback) mtlr_tex_download(o->tex, face, level, G2H(o->lock_ptr), pitch);
    }
    o->lock_pitch = rd32(lr); o->lock_index = (uint32_t)(face << 8 | level);
    o->lock_data = rd32(lr + 4);
}
static void lock_end(RnObj *o) {
    if (!o->lock_data) return;
    if (o->lock_box_slice) {   // volume
        mtlr_tex_upload(o->tex, 0, (int)o->lock_index, G2H(o->lock_data), o->lock_pitch, o->lock_box_slice);
        o->lock_data = 0; o->lock_box_slice = 0; return;
    }
    int face = (int)(o->lock_index >> 8), level = (int)(o->lock_index & 0xFF);
    if (getenv("RT_RN_UPLOADS")) rt_log("[rn] upload tex %u face %d level %d rect %d,%d %dx%d pitch %u data %08x", o->tex, face, level,
                                        o->lock_rect[0], o->lock_rect[1], o->lock_rect[2], o->lock_rect[3], o->lock_pitch, o->lock_data);
    if (getenv("RT_RN_UPLOADS")) { const float *f = (const float *)G2H(o->lock_data);
        rt_log("[rn]   data %g %g %g %g | %g %g %g %g | %g %g %g %g", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]); }
    if (o->lock_rect[2]) mtlr_tex_upload_rect(o->tex, face, level, o->lock_rect[0], o->lock_rect[1], o->lock_rect[2], o->lock_rect[3], G2H(o->lock_data), o->lock_pitch);
    else mtlr_tex_upload(o->tex, face, level, G2H(o->lock_data), o->lock_pitch, 0);
    o->lock_data = 0;
}
// LockRect(this, level, D3DLOCKED_RECT *, rect, flags) / cube: LockRect(this, face, level, *, rect, flags)
void N_s_lock(CPU *c) {
    uint32_t t = ARG(0), level = ARG(1), lr = ARG(2), rect = ARG(3);
    if (SHADOW) forward(c, 0x4c);
    RnObj *o = rn_get(t);
    if (o && o->tex) lock_begin(o, 0, (int)level, lr, rect);
    if (!SHADOW) RETURN(0, 5);
}
void N_s_lock_cube(CPU *c) {
    uint32_t t = ARG(0), face = ARG(1), level = ARG(2), lr = ARG(3), rect = ARG(4);
    if (SHADOW) forward(c, 0x4c);
    RnObj *o = rn_get(t);
    if (o && o->tex) lock_begin(o, (int)face, (int)level, lr, rect);
    if (!SHADOW) RETURN(0, 6);
}
void N_s_unlock(CPU *c) {       // UnlockRect(this, level)
    RnObj *o = rn_get(ARG(0));
    if (o) lock_end(o);
    if (SHADOW) { forward(c, 0x50); return; }
    RETURN(0, 2);
}
void N_s_unlock_cube(CPU *c) {  // UnlockRect(this, face, level)
    RnObj *o = rn_get(ARG(0));
    if (o) lock_end(o);
    if (SHADOW) { forward(c, 0x50); return; }
    RETURN(0, 3);
}
// volume textures: CreateVolumeTexture(this, w, h, d, levels, usage, fmt, pool, **tex, shared); LockBox(this, level,
// D3DLOCKED_BOX {row pitch, slice pitch, bits} *, box, flags); unlocks go through N_s_unlock (same slot and arguments)
void N_s_create_volume(CPU *c) {
    uint32_t w = ARG(1), h = ARG(2), dp = ARG(3), lv = ARG(4), us = ARG(5), fmt = ARG(6), out = ARG(8);
    if (SHADOW) forward(c, 0x60);
    uint32_t t = created(c, out, TK_TEX);
    if (t) {
        MtlTexDesc d; memset(&d, 0, sizeof d);
        d.type = MTLR_TEX_3D; d.width = w; d.height = h; d.depth = dp; d.samples = 1; d.levels = (uint8_t)(lv ? lv : 1);
        d.format = rn_pixel_format(fmt, &d.swizzle); d.usage = MTLR_USE_SAMPLE | ((us & 1) ? MTLR_USE_TARGET : 0);
        RnObj *o = rn_put(t, RN_TEX); o->tex = d.format ? mtlr_tex_create(&d) : 0;
    }
    if (!SHADOW) RETURN(0, 10);
}
void N_s_lock_box(CPU *c) {
    uint32_t t = ARG(0), level = ARG(1), lb = ARG(2);
    if (SHADOW) forward(c, 0x4c);
    RnObj *o = rn_get(t);
    if (o && o->tex) {
        uint32_t pitch, rows, size = mtlr_tex_level_size(o->tex, (int)level, &pitch, &rows);
        if (!SHADOW) {
            if (!o->lock_ptr || o->lock_slice < size) { o->lock_ptr = rn_galloc(size); o->lock_slice = size; }
            wr32(lb, pitch); wr32(lb + 4, pitch * rows); wr32(lb + 8, o->lock_ptr);
        }
        o->lock_pitch = rd32(lb); o->lock_index = level; o->lock_data = rd32(lb + 8); o->lock_rect[2] = 0;
        o->lock_box_slice = rd32(lb + 4);
    }
    if (!SHADOW) RETURN(0, 5);
}
// GetCubeMapSurface(this, face, level, **surface)
void N_s_cube_surface(CPU *c) {
    uint32_t t = ARG(0), face = ARG(1), level = ARG(2), out = ARG(3);
    if (SHADOW) forward(c, 0x48);
    uint32_t s = created(c, out, TK_SURF);
    RnObj *to = rn_get(t);
    if (s && to) { RnObj *o = rn_put(s, RN_SURF); o->tex = to->tex; o->face = (uint8_t)face; o->level = (uint8_t)level; }
    if (!SHADOW) RETURN(0, 4);
}
// CreateRenderTarget(this, w, h, fmt, samples, quality, lockable, **surface, shared): its own (multisampled) target
void N_s_create_rt(CPU *c) {
    uint32_t w = ARG(1), h = ARG(2), fmt = ARG(3), ms = ARG(4), out = ARG(7);
    if (SHADOW) forward(c, 0x70);
    uint32_t s = created(c, out, TK_SURF);
    if (s) {
        uint8_t swz; MtlTexDesc d = {MTLR_TEX_2D, 1, (uint8_t)(ms > 1 ? ms : 1), MTLR_USE_TARGET | MTLR_USE_SAMPLE, 0, rn_pixel_format(fmt, &swz), w, h, 1};
        create_log("render target", d.width, d.height);
        RnObj *o = rn_put(s, RN_SURF); o->tex = d.format ? mtlr_tex_create(&d) : 0; o->face = o->level = 0; o->owns = 1; o->used_frame = rn_frame;
    }
    if (!SHADOW) RETURN(0, 9);
}
void N_s_set_rt(CPU *c) { if (SHADOW) { forward(c, 0x94); return; } RETURN(0, 3); }   // SetRenderTarget(this, index, surface)
// readback (F_9dfb50 on render-target textures): GetDesc(surface, D3DSURFACE_DESC *), GetRenderTargetData(this, src, dst)
void N_s_surface_desc(CPU *c) {
    uint32_t s = ARG(0), p = ARG(1);
    if (SHADOW) { forward(c, 0x30); return; }
    RnObj *o = rn_get(s);
    const MtlTexDesc *d = o ? mtlr_tex_desc(o->backbuffer ? mtlr_backbuffer() : o->tex) : NULL;
    memset(G2H(p), 0, 32);
    if (d) { wr32(p, 21); wr32(p + 4, 1); wr32(p + 8, 1); wr32(p + 24, MAX(1u, d->width >> (o ? o->level : 0))); wr32(p + 28, MAX(1u, d->height >> (o ? o->level : 0))); }
    RETURN(0, 2);
}
void N_s_rt_data(CPU *c) {
    uint32_t src = ARG(1), dst = ARG(2);
    if (SHADOW) { forward(c, 0x80); return; }
    RnObj *a = rn_get(src), *b = rn_get(dst);
    if (a && b) {
        MtlTex at = a->backbuffer ? mtlr_backbuffer() : a->tex;
        const MtlTexDesc *ad = mtlr_tex_desc(at), *bd = mtlr_tex_desc(b->tex);
        if (ad && bd) { int r[4] = {0, 0, (int)MIN(ad->width, bd->width), (int)MIN(ad->height, bd->height)}; mtlr_copy(at, r, b->tex, r, 0); }
        rn_mark_readback(b->tex);   // the destination's texture object downloads on its next lock
    }
    RETURN(0, 3);
}

// GetSurfaceLevel(this, level, **surface)
void N_s_surface_level(CPU *c) {
    uint32_t t = ARG(0), level = ARG(1), out = ARG(2);
    if (SHADOW) forward(c, 0x48);
    uint32_t s = created(c, out, TK_SURF);
    RnObj *to = rn_get(t);
    if (s && to) { RnObj *o = rn_put(s, RN_SURF); o->tex = to->tex; o->face = 0; o->level = (uint8_t)level; }
    if (!SHADOW) RETURN(0, 3);
}
// CreateDepthStencilSurface(this, w, h, fmt, samples, quality, discard, **surface, shared)
void N_s_create_depth(CPU *c) {
    uint32_t w = ARG(1), h = ARG(2), fmt = ARG(3), out = ARG(7);
    if (SHADOW) forward(c, 0x74);
    uint32_t s = created(c, out, TK_SURF);
    if (s) {
        uint8_t swz; MtlTexDesc d = {MTLR_TEX_2D, 1, 1, MTLR_USE_TARGET | MTLR_USE_SAMPLE, 0, rn_pixel_format(fmt, &swz), w, h, 1};
        if (!d.format) d.format = MTLR_PF_D32F_S8;
        create_log("depth surface", d.width, d.height);
        RnObj *o = rn_put(s, RN_SURF); o->tex = mtlr_tex_create(&d); o->face = o->level = 0; o->owns = 1; o->used_frame = rn_frame;
    }
    if (!SHADOW) RETURN(0, 9);
}

// ---------------- vertex / index buffers ----------------
// the data lives in guest memory the GPU reads directly. Discarding locks on dynamic buffers rename: the draws
// recorded so far keep the old block, the engine writes a new one (blocks are recycled after FRAMES+1 frames).
#define FRAMES_BUSY 4              // a block retired at frame F is free once frame F + 4 starts (3 in flight + 1 ahead)
static void buffer_created(uint32_t b, uint32_t len, uint32_t usage) {
    RnObj *o = rn_put(b, RN_BUF);
    o->size = len; o->dynamic = (usage & 0x200) != 0;   // D3DUSAGE_DYNAMIC
    o->addr = rn_galloc(len);
    o->blk[0] = o->addr; o->blk_retired[0] = 0xFFFFFFFFu; o->nblk = 1; o->cur = 0;
}
// D3DLOCK_DISCARD on a dynamic buffer: the draws recorded so far keep the current block; the engine gets a block
// the GPU has finished with (or a new one), like D3D's buffer renaming
static void buffer_discard(RnObj *o) {
    o->blk_retired[o->cur] = present_count;
    int pick = -1;
    for (int i = 0; i < o->nblk; i++)
        if (i != o->cur && o->blk_retired[i] != 0xFFFFFFFFu && present_count - o->blk_retired[i] >= FRAMES_BUSY) { pick = i; break; }
    if (pick < 0 && o->nblk < 8) { pick = o->nblk++; o->blk[pick] = rn_galloc(o->size); }
    if (pick < 0) {   // all 8 busy: the oldest (a hazard only if the engine discards more than 8 times in 3 frames)
        pick = o->cur == 0 ? 1 : 0;
        for (int i = 0; i < 8; i++) if (i != o->cur && o->blk_retired[i] < o->blk_retired[pick]) pick = i;
    }
    o->blk_retired[pick] = 0xFFFFFFFFu; o->cur = (uint8_t)pick; o->addr = o->blk[pick];
}
// CreateVertexBuffer(this, length, usage, fvf, pool, **vb, shared) / CreateIndexBuffer(this, length, usage, fmt, pool, **ib, shared)
void N_s_create_vb(CPU *c) {
    uint32_t len = ARG(1), us = ARG(2), out = ARG(5);
    if (SHADOW) forward(c, 0x68);
    uint32_t b = created(c, out, TK_VB);
    if (b) buffer_created(b, len, us);
    if (!SHADOW) RETURN(0, 7);
}
void N_s_create_ib(CPU *c) {
    uint32_t len = ARG(1), us = ARG(2), fmt = ARG(3), out = ARG(5);
    if (SHADOW) forward(c, 0x6c);
    uint32_t b = created(c, out, TK_IB);
    if (b) { buffer_created(b, len, us); rn_get(b)->index32 = fmt == 0x66; }
    if (!SHADOW) RETURN(0, 7);
}
// Lock(this, offset, size, **data, flags)
void N_s_buf_lock(CPU *c) {
    uint32_t b = ARG(0), off = ARG(1), out = ARG(3), fl = ARG(4);
    if (SHADOW) forward(c, 0x2c);
    RnObj *o = rn_get(b);
    if (o) {
        if (o->dynamic && (fl & 0x2000)) buffer_discard(o);   // D3DLOCK_DISCARD: a fresh block
        else if (!(fl & 0x1010)) mtlr_wait_seq(o->used_seq);  // no NOOVERWRITE / READONLY: as D3D9, wait for the GPU
        if (SHADOW) { o->lock_data = rd32(out) - off; }   // the staging copy, mirrored at unlock
        else wr32(out, o->addr + off);
    }
    if (!SHADOW) RETURN(0, 5);
}
void N_s_buf_unlock(CPU *c) {   // Unlock(this)
    RnObj *o = rn_get(ARG(0));
    if (o && SHADOW && o->lock_data) { memcpy(G2H(o->addr), G2H(o->lock_data), o->size); o->lock_data = 0; }
    if (SHADOW) { forward(c, 0x30); return; }
    RETURN(0, 1);
}

// ---------------- shaders and declarations ----------------
static void shader(CPU *c, uint32_t code, uint32_t out, int kind, uint32_t off) {
    rn_setup();
    if (SHADOW) forward(c, off);
    uint32_t s = created(c, out, kind == RN_VS ? TK_VS : TK_PS);
    if (s) {
        const uint32_t *t = (const uint32_t *)G2H(code);
        RnObj *o = rn_put(s, (uint8_t)kind); o->hash = mtlr_shader_add(t, sm3_length(t));
    }
    if (!SHADOW) RETURN(0, 3);
}
void N_s_create_vs(CPU *c) { shader(c, ARG(1), ARG(2), RN_VS, 0x16c); }   // CreateVertexShader(this, code, **shader)
void N_s_create_ps(CPU *c) { shader(c, ARG(1), ARG(2), RN_PS, 0x1a8); }   // CreatePixelShader(this, code, **shader)
// CreateVertexDeclaration(this, D3DVERTEXELEMENT9 *, **decl): {stream, offset (16), type, method, usage, index (8)}
uint8_t rn_vertex_format(uint8_t d3d);   // rn_res.c
void N_s_create_decl(CPU *c) {
    uint32_t el = ARG(1), out = ARG(2);
    MtlVertexElement e[32]; int n = 0;
    for (; n < 32; n++, el += 8) {
        uint32_t lo = rd32(el), hi = rd32(el + 4);
        if ((lo & 0xFFFF) == 0xFF) break;
        e[n].stream = (uint8_t)lo; e[n].offset = (uint8_t)(lo >> 16); e[n].format = rn_vertex_format((uint8_t)hi);
        e[n].usage = (uint8_t)(hi >> 16); e[n].index = (uint8_t)(hi >> 24);
    }
    if (SHADOW) forward(c, 0x158);
    uint32_t d = created(c, out, TK_DECL);
    if (d) { RnObj *o = rn_put(d, RN_DECL); o->layout = mtlr_vertex_layout(e, n); }
    if (!SHADOW) RETURN(0, 3);
}
