#include <time.h>
// sRender's draw packet replay (F_7bc8a0) on Metal: the packet walk and the engine's state objects decoded straight
// into the Metal backend. The field layouts are noted where each decoder reads them.
#include <string.h>
#include "rn.h"

int rn_trace;   // RT_RN_TRACE: log the packets of one frame
int rn_stat[8];   // draws, indexed, clears, copies, skipped (no shader), no vs/ps object, instanced, packets
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// ---------------- targets ----------------
static int tw = 1, th = 1;     // current target size
static MtlTex auto_depth;
static MtlTarget surface_target_kind(uint32_t s, int *w, int *h, int depth);
static MtlTarget surface_target(uint32_t s, int *w, int *h) { return surface_target_kind(s, w, h, 0); }
static MtlTarget surface_target_kind(uint32_t s, int *w, int *h, int depth) {
    MtlTarget t = {0};
    if (!s) return t;
    uint32_t slot = rd32(s + 0x14);
    RnObj *o = rn_get(slot);
    uint32_t wh = rd32(s + 0x18);
    if (w) { *w = (int)(wh & 0xFFFF); *h = (int)((wh >> 16) & 0x7FFF); }
    if (o && o->kind == RN_SURF) {
        o->used_frame = rn_frame;   // an owned surface the game released stays while draws use it (rn_sites.c)
        t.tex = o->backbuffer ? mtlr_backbuffer() : o->tex; t.face = o->face; t.level = o->level; return t;
    }
    if (slot) {   // not one we made: the swap chain's back buffer or its automatic depth buffer
        static uint32_t seen[8]; int known = 0;
        for (int i = 0; i < 8; i++) if (seen[i] == slot) known = 1;
        if (!known) { for (int i = 0; i < 8; i++) if (!seen[i]) { seen[i] = slot; break; }
            rt_log("[rn] surface %08x (slot %08x, %ux%u) -> swap chain %s", s, slot, wh & 0xFFFF, (wh >> 16) & 0x7FFF, depth ? "depth" : "back buffer"); }
        if (depth) {
            const MtlTexDesc *bd = mtlr_tex_desc(mtlr_backbuffer());
            if (!auto_depth && bd) {
                MtlTexDesc d = {MTLR_TEX_2D, 1, 1, MTLR_USE_TARGET | MTLR_USE_SAMPLE, 0, MTLR_PF_D32F_S8, bd->width, bd->height, 1};
                auto_depth = mtlr_tex_create(&d);
            }
            t.tex = auto_depth;
        } else t.tex = mtlr_backbuffer();
    }
    return t;
}
// target set: +0..0xc color surfaces, +0x10 depth surface, +0x14 flags (bit 9: second rect set), +0x18/+0x28
// viewport rects, +0x38/+0x48 scissor rects {x0, y0, x1, y1}, +0x58 size (w | h << 16)
static int scissor_rect[4];
static void apply_targets(uint32_t T) {
    if (!T) return;
    MtlTarget c[4], d; int w = 0, h = 0;
    for (int i = 0; i < 4; i++) c[i] = surface_target(rd32(T + 4u * (uint32_t)i), i ? NULL : &w, &h);
    int dw = 0, dh = 0;
    d = surface_target_kind(rd32(T + 0x10), &dw, &dh, 1);
    if (!c[0].tex && d.tex) { w = dw; h = dh; }
    mtlr_set_targets(c, d);
    if (rn_trace) rt_log("[rn]    targets T %08x: color0 surf %08x slot %08x -> tex %u (bb %u), depth tex %u", T, rd32(T), rd32(T) ? rd32(rd32(T) + 0x14) : 0, c[0].tex, mtlr_backbuffer(), d.tex);
    uint32_t sel = (rd32(T + 0x14) >> 9) & 1, size = rd32(T + 0x58);
    int maxw = (int)(size & 0xFFFF), maxh = (int)(size >> 16);
    if (!maxw) maxw = w; if (!maxh) maxh = h;
    uint32_t r = T + 0x18 + 16 * sel;
    int x0 = (int)rd32(r), y0 = (int)rd32(r + 4), x1 = (int)rd32(r + 8), y1 = (int)rd32(r + 12);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > maxw) x1 = maxw; if (y1 > maxh) y1 = maxh;
    if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
    mtlr_set_viewport((float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0), 0, 1);
    uint32_t s = T + 0x38 + 16 * sel;
    int sx0 = (int)rd32(s), sy0 = (int)rd32(s + 4), sx1 = (int)rd32(s + 8), sy1 = (int)rd32(s + 12);
    if (sx0 < x0) sx0 = x0; if (sy0 < y0) sy0 = y0; if (sx1 > x1) sx1 = x1; if (sy1 > y1) sy1 = y1;
    if (sx1 < sx0 || sy1 < sy0) sx0 = sx1 = sy0 = sy1 = 0;
    scissor_rect[0] = sx0; scissor_rect[1] = sy0; scissor_rect[2] = sx1; scissor_rect[3] = sy1;
    tw = w ? w : 1; th = h ? h : 1;
}

// ---------------- render state block ----------------
static const uint8_t blend_factor[16] = {0, 0, 1, 2, 3, 4, 5, 8, 9, 6, 7, 10, 4, 5, 11, 12};   // D3DBLEND -> MTLBlendFactor
static uint8_t mask_of(uint32_t d3d) { return (uint8_t)(((d3d & 1) << 3) | ((d3d & 2) << 1) | ((d3d & 4) >> 1) | ((d3d & 8) >> 3)); }
static uint8_t cmp_of(uint32_t d3d) { return (uint8_t)(d3d >= 1 && d3d <= 8 ? d3d - 1 : 7); }
static uint8_t sop_of(uint32_t d3d) { return (uint8_t)(d3d >= 1 && d3d <= 8 ? d3d - 1 : 0); }
static uint8_t bop_of(uint32_t d3d) { return (uint8_t)(d3d >= 1 && d3d <= 5 ? d3d - 1 : 0); }
static int scissor_on;

// R: +0 blend factor (ARGB), +4 stencil ref (byte 0) | rasterizer variant (bit 8), +8 blend state (+0x20),
// +0xc depth-stencil state (+0x18), +0x10 rasterizer state (+0x20 / +0x2c)
static void apply_state(uint32_t R) {
    if (!R) return;
    uint32_t bf = rd32(R);
    float f[4] = {((bf >> 16) & 0xFF) / 255.f, ((bf >> 8) & 0xFF) / 255.f, (bf & 0xFF) / 255.f, (bf >> 24) / 255.f};
    MtlBlend b[4]; memset(b, 0, sizeof b);
    uint32_t bs = rd32(R + 8);
    uint32_t w0 = bs ? rd32(bs + 0x20) : 0x78000000u, w1 = bs ? rd32(bs + 0x24) : 0xFFF;
    for (int i = 0; i < 4; i++) {
        b[i].enable = w0 & 1;
        b[i].src = blend_factor[(w0 >> 1) & 0xF]; b[i].dst = blend_factor[(w0 >> 6) & 0xF]; b[i].op = bop_of((w0 >> 11) & 7);
        b[i].src_a = blend_factor[(w0 >> 14) & 0xF]; b[i].dst_a = blend_factor[(w0 >> 19) & 0xF]; b[i].op_a = bop_of((w0 >> 24) & 7);
        b[i].mask = mask_of(i ? (w1 >> (4 * (i - 1))) & 0xF : (w0 >> 27) & 0xF);
    }
    mtlr_set_blend(b, f);
    MtlDepthStencil ds; memset(&ds, 0, sizeof ds);
    uint32_t dso = rd32(R + 0xc);
    if (dso) {
        uint32_t d0 = rd32(dso + 0x18), d1 = rd32(dso + 0x1c);
        ds.depth_test = d0 & 1; ds.depth_write = (d0 >> 1) & 1; ds.depth_func = cmp_of((d0 >> 2) & 0xF);
        ds.stencil = (d0 >> 6) & 1;
        ds.read_mask = (uint8_t)(d0 >> 8); ds.write_mask = (uint8_t)(d0 >> 16);
        ds.front = (MtlStencilFace){sop_of((d0 >> 24) & 0xF), sop_of(d1 & 0xF), sop_of(d0 >> 28), cmp_of((d1 >> 4) & 0xF)};
        ds.back = ((d0 >> 7) & 1) ? (MtlStencilFace){sop_of((d1 >> 8) & 0xF), sop_of((d1 >> 16) & 0xF), sop_of((d1 >> 12) & 0xF), cmp_of((d1 >> 20) & 0xF)} : ds.front;
    } else { ds.depth_test = 1; ds.depth_write = 1; ds.depth_func = 3; }
    ds.ref = (uint8_t)rd32(R + 4);
    mtlr_set_depth_stencil(&ds);
    MtlRaster rs = {0, 0, 0, 0, 0};
    uint32_t ro = rd32(R + 0x10);
    scissor_on = 0;
    if (ro) {
        uint32_t r = ro + 0x20 + 12 * ((rd32(R + 4) >> 8) & 1);
        uint32_t r0 = rd32(r);
        uint32_t cull = (r0 >> 8) & 0xFF, fill = r0 & 0xFF;
        rs.cull = cull == 2 ? 1 : cull == 3 ? 2 : 0;       // D3D: front faces are clockwise
        rs.fill = fill == 2 ? 1 : 0;
        rs.depth_bias = u2f(rd32(r + 4)) * 16777216.f; rs.slope_bias = u2f(rd32(r + 8));
        scissor_on = (r0 >> 17) & 1;
    }
    mtlr_set_raster(&rs);
}
static void apply_scissor(void) {
    if (scissor_on) mtlr_set_scissor(scissor_rect[0], scissor_rect[1], scissor_rect[2] - scissor_rect[0], scissor_rect[3] - scissor_rect[1]);
    else mtlr_set_scissor(0, 0, tw, th);
}

// ---------------- streams ----------------
// S: +0..0xc vertex buffer objects (+0x20 slot), +0x10..0x13 strides, +0x14..0x20 offsets, +0x28 index buffer
static void apply_streams(uint32_t S) {
    if (!S) return;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t vb = rd32(S + 4 * i);
        RnObj *o = vb ? rn_get(rd32(vb + 0x20)) : NULL;
        if (o) o->used_seq = mtlr_seq_now();
        mtlr_set_stream((int)i, o ? o->addr + rd32(S + 0x14 + 4 * i) : 0, rd8(S + 0x10 + i), 0);
    }
    uint32_t ib = rd32(S + 0x28);
    RnObj *o = ib ? rn_get(rd32(ib + 0x20)) : NULL;
    if (o) o->used_seq = mtlr_seq_now();
    mtlr_set_indices(o ? o->addr : 0, 2);
}

// ---------------- shader, constants, textures ----------------
static const uint8_t addr_mode[8] = {2, 2, 3, 0, 5, 1, 2, 2};   // D3DTADDRESS -> MTLSamplerAddressMode
static void sampler_of(uint32_t so, MtlSampler *s) {
    memset(s, 0, sizeof *s);
    s->u = s->v = s->w = 2; s->min = s->mag = 1; s->mip = 2;
    if (!so) return;
    uint32_t w0 = rd32(so + 0x30), border = rd32(so + 0x34);
    s->u = addr_mode[w0 & 7]; s->v = addr_mode[(w0 >> 3) & 7]; s->w = addr_mode[(w0 >> 6) & 7];
    uint32_t mag = (w0 >> 9) & 0xF, min = (w0 >> 13) & 0xF, mip = (w0 >> 17) & 0xF;
    s->mag = mag >= 2; s->min = min >= 2; s->mip = (uint8_t)(mip == 0 ? 0 : mip == 1 ? 1 : 2);
    s->aniso = min == 3 ? (uint8_t)((w0 >> 21) & 0x1F) : 1;
    s->max_level = (float)((w0 >> 26) & 0xF);
    s->lod_bias = u2f(rd32(so + 0x38));
    s->border = border == 0 ? 0 : (border >> 24) == 0xFF && (border & 0xFFFFFF) == 0 ? 1 : 2;
}
// one stage's section of the program's binding list: textures, float constants, int, bool, texture sizes
static void bind_section(int stage, uint32_t M, uint32_t list, uint32_t counts) {
    uint32_t n_tex = (counts >> 8) & 0xF, n_f = (counts >> 12) & 0x7F, n_i = (counts >> 19) & 0xF, n_b = (counts >> 23) & 0x1F, n_sz = counts >> 28;
    if (rn_trace) rt_log("[rn]    %s counts %08x: tex %u f %u i %u b %u size %u", stage ? "ps" : "vs", counts, n_tex, n_f, n_i, n_b, n_sz);
    uint32_t d = list;
    int bound[16] = {0};
    int srgb_global = rd8(SRENDER + 0x31) != 0;
    for (uint32_t k = 0; k < n_tex; k++, d += 4) {
        uint32_t e = rd32(d), slot = e & 0xFFF;
        uint32_t so = rd32(M + 0xc + 4 * ((e >> 12) & 0x3FF)), to = rd32(M + 0xc + 4 * (e >> 22));
        if (slot >= 16) continue;
        MtlSampler s; sampler_of(so, &s);
        RnObj *o = to ? rn_get(rd32(to + 0x34)) : NULL;
        if (stage == SM3_STAGE_PIXEL && srgb_global && to && ((rd32(to + 0x20) >> 30) & 1) && !(rd32(to + 0x2c) & 0x80000)) s.srgb = 1;
        mtlr_set_texture(stage, (int)slot, o ? o->tex : 0, &s);
        if (rn_trace) rt_log("[rn]    %s tex s%u: obj %08x slot %08x -> %u", stage ? "ps" : "vs", slot, to, to ? rd32(to + 0x34) : 0, o ? o->tex : 0);
        bound[slot] = 1;
    }
    for (int i = 0; i < (stage ? 16 : 4); i++) if (!bound[i]) mtlr_set_texture(stage, i, 0, NULL);
    for (uint32_t k = 0; k < n_f; k++, d += 4) {
        uint32_t e = rd32(d), raw = rd32(M + 0xc + 4 * ((e >> 1) & 0x1F));
        uint32_t src = (raw & ~0xFu) + 4 * ((e >> 6) & 0x3FF), start = (e >> 16) & 0xFF, count = e >> 24;
        if (raw & 0xF) { uint32_t lim = (raw & 0xF) << 4; if (lim < count) count = lim; }
        if (!src || !count) continue;
        if (e & 1) {   // integers converted to floats
            float v[4 * 64];
            if (count > 64) count = 64;
            for (uint32_t r = 0; r < count * 4; r++) v[r] = (float)(int32_t)rd32(src + 4 * r);
            mtlr_set_consts(stage, (int)start, v, (int)count);
        } else mtlr_set_consts(stage, (int)start, (const float *)G2H(src), (int)count);
        if (rn_trace && stage == SM3_STAGE_VERTEX) {
            const float *f = (const float *)G2H(src);
            rt_log("[rn]    vs c%u..c%u (raw %08x int %u): %g %g %g %g | %g %g %g %g", start, start + count - 1, raw, e & 1, f[0], f[1], f[2], f[3], count > 1 ? f[4] : 0, count > 1 ? f[5] : 0, count > 1 ? f[6] : 0, count > 1 ? f[7] : 0);
        }
    }
    d += 4 * (n_i + n_b);   // the converted shaders read no int or bool constants
    for (uint32_t k = 0; k < n_sz; k++, d += 4) {
        uint32_t e = rd32(d), kind = (e >> 8) & 0xFF, reg = (e >> 16) & 0xFF;
        uint32_t to = rd32(M + 0xc + 4 * ((e >> 1) & 0x7F));
        if (rn_trace) rt_log("[rn]    %s texsize entry %08x kind %u reg %u count %u tex obj %08x", stage ? "ps" : "vs", e, kind, reg, e >> 24, to);
        if (!to || (kind != 3 && kind != 7)) continue;
        float v[4] = {(float)(rd32(to + 0x20) & 0xFFFF), (float)((rd32(to + 0x20) >> 16) & 0x3FFF), (float)(rd32(to + 0x24) >> 16 & 0x7FF), 0};
        v[2] = (float)(((uint32_t)rd16(to + 0x26)) & 0x7FF);
        if (kind == 3) for (int i = 0; i < 3; i++) v[i] = v[i] ? 1.f / v[i] : 0;
        mtlr_set_consts(stage, (int)reg, v, 1);
    }
}
// M: +0 flags (alpha test: bits 19-20 == 3, func index bits 3-10 (table at 0xc853a8), ref bits 11-18; primitive
// type bits 21-28), +8 program, +0xc resources. Program: +4 binding list, +8 / +0xc vertex / pixel counts,
// +0x10 vertex shader, +0x14 pixel shader, +0x18 declaration
static int prim_of(uint32_t M) { uint32_t p = (rd32(M) >> 21) & 0xFF; return p <= 4 ? (int)p : 0; }
static int apply_shader(uint32_t M) {
    if (!M) return 0;
    uint32_t f = rd32(M), P = rd32(M + 8);
    if (!P) return 0;
    if ((f & 0x180000) == 0x180000) mtlr_set_alpha_test(cmp_of(rd32(0xc853a8u + 4 * ((f >> 3) & 0xFF))), (int)((f >> 11) & 0xFF));
    else mtlr_set_alpha_test(7, 0);
    RnObj *vs = rn_get(rd32(P + 0x10)), *ps = rn_get(rd32(P + 0x14)), *dl = rn_get(rd32(P + 0x18));
    if (!vs || !ps) { rn_stat[5]++; return 0; }
    mtlr_set_shaders(vs->hash, ps->hash, dl ? dl->layout : 0);
    if (rn_trace) rt_log("[rn]    shaders vs_%08x ps_%08x layout %u", vs->hash, ps->hash, dl ? dl->layout : 0);
    uint32_t list = rd32(P + 4), vc = rd32(P + 8), pc = rd32(P + 0xc);
    bind_section(SM3_STAGE_VERTEX, M, list, vc);
    bind_section(SM3_STAGE_PIXEL, M, list + 4 * (vc & 0xFF), pc);
    return 1;
}

// ---------------- packets ----------------
static int draw_setup(uint32_t pk) {
    apply_targets(rd32(pk + 4));
    apply_state(rd32(pk + 8));
    apply_scissor();
    if (rn_trace) {
        uint32_t T = rd32(pk + 4), S = rd32(pk + 0x10);
        rt_log("[rn]  draw pk %08x T %08x size %dx%d vp sel %u rect0 %d,%d,%d,%d rect1 %d,%d,%d,%d max %08x scissor_on %d sc %d,%d,%d,%d vb0 %08x stride %u count %u",
               pk, T, tw, th, (rd32(T + 0x14) >> 9) & 1, rd32(T + 0x18), rd32(T + 0x1c), rd32(T + 0x20), rd32(T + 0x24),
               rd32(T + 0x28), rd32(T + 0x2c), rd32(T + 0x30), rd32(T + 0x34), rd32(T + 0x58), scissor_on,
               scissor_rect[0], scissor_rect[1], scissor_rect[2], scissor_rect[3], S ? rd32(S) : 0, S ? rd8(S + 0x10) : 0, rd32(pk + 0x14));
        uint32_t vb = S ? rd32(S) : 0; RnObj *o = vb ? rn_get(rd32(vb + 0x20)) : NULL;
        uint32_t a = o ? o->addr + rd32(S + 0x14) : 0;
        rt_log("[rn]    vb slot %08x obj %p addr %08x size %u first %g %g %g %g | start %u", vb ? rd32(vb + 0x20) : 0, (void *)o, a, o ? o->size : 0,
               a ? u2f(rd32(a)) : 0, a ? u2f(rd32(a + 4)) : 0, a ? u2f(rd32(a + 8)) : 0, a ? u2f(rd32(a + 12)) : 0, rd32(pk + 0x18));
    }
    apply_streams(rd32(pk + 0x10));
    return apply_shader(rd32(pk + 0xc));
}
static void packet(uint32_t pk) {
    uint32_t type = rd16(pk + 2) & 0xF;
    switch (type) {
    case 0:   // draw
        if (draw_setup(pk)) { rn_stat[0]++; mtlr_draw(prim_of(rd32(pk + 0xc)), rd32(pk + 0x18), rd32(pk + 0x14), 1); } else rn_stat[4]++;
        break;
    case 1:   // indexed draw
        if (rn_trace) {
            uint32_t S = rd32(pk + 0x10), ib = S ? rd32(S + 0x28) : 0; RnObj *io = ib ? rn_get(rd32(ib + 0x20)) : NULL;
            uint32_t first = rd32(pk + 0x18), a = io ? io->addr + 2 * first : 0;
            rt_log("[rn]  indexed pk %08x count %u first %u base %d ib %08x obj %p addr %08x idx %u %u %u", pk, rd32(pk + 0x14), first, (int32_t)rd32(pk + 0x1c),
                   ib, (void *)io, a, a ? rd16(a) : 0, a ? rd16(a + 2) : 0, a ? rd16(a + 4) : 0);
        }
        if (draw_setup(pk)) { rn_stat[1]++; mtlr_draw_indexed(prim_of(rd32(pk + 0xc)), rd32(pk + 0x18), rd32(pk + 0x14), (int32_t)rd32(pk + 0x1c), 1); }
        else rn_stat[4]++;
        break;
    case 2: case 3: {
        static int logged; if (!logged++) rt_log("[rn] instanced draw packets are not implemented yet");
        rn_stat[6]++;
        break;
    }
    case 4: {   // clear: +0x20 {+4 targets, +8 state}, +4 flags (0-3 color targets, 4 depth, 5 stencil), +8 depth,
                // +0xc stencil, +0x10 color
        uint32_t s = rd32(pk + 0x20);
        if (s) { apply_targets(rd32(s + 4)); apply_state(rd32(s + 8)); }
        uint32_t fl = rd32(pk + 4);
        float col[4] = {u2f(rd32(pk + 0x10)), u2f(rd32(pk + 0x14)), u2f(rd32(pk + 0x18)), u2f(rd32(pk + 0x1c))};
        int flags = ((fl & 0xF) ? MTLR_CLEAR_COLOR : 0) | ((fl & 0x10) ? MTLR_CLEAR_DEPTH : 0) | ((fl & 0x20) ? MTLR_CLEAR_STENCIL : 0);
        mtlr_clear(flags, col, u2f(rd32(pk + 8)), (uint8_t)rd32(pk + 0xc)); rn_stat[2]++;
        break;
    }
    case 5: {   // copy: +0x28 {+4 targets}, +4 destination texture, +8 its surface index, +0xc source (0-3 color target,
                // else depth), +0x10 source rect, +0x20 destination offset
        uint32_t s = rd32(pk + 0x28);
        if (s) apply_targets(rd32(s + 4));
        uint32_t T = s ? rd32(s + 4) : 0, dst_tex = rd32(pk + 4), idx = rd32(pk + 8), src = rd32(pk + 0xc) & 0xF;
        if (!T || !dst_tex) break;
        uint32_t src_surf = src < 4 ? rd32(T + 4 * src) : rd32(T + 0x10);
        MtlTarget a = surface_target_kind(src_surf, NULL, NULL, src >= 4);
        RnObj *d = rn_get(rd32(dst_tex + 0x34));
        if (!a.tex || !d) break;
        (void)idx;
        int sr[4] = {(int)rd32(pk + 0x10), (int)rd32(pk + 0x14), (int)rd32(pk + 0x18), (int)rd32(pk + 0x1c)};
        int dx = (int)rd32(pk + 0x20), dy = (int)rd32(pk + 0x24);
        int dr[4] = {dx, dy, dx + sr[2] - sr[0], dy + sr[3] - sr[1]};
        if (src >= 4) { const MtlTexDesc *sd = mtlr_tex_desc(a.tex); if (sd) { sr[0] = sr[1] = 0; sr[2] = (int)sd->width; sr[3] = (int)sd->height; dr[0] = dr[1] = 0; dr[2] = sr[2]; dr[3] = sr[3]; } }
        mtlr_copy(a.tex, sr, d->tex, dr, 0); rn_stat[3]++;
        if (rn_trace) rt_log("[rn]    copy tex %u -> tex %u (src %u)", a.tex, d->tex, src);
        break;
    }
    default: break;   // 6 (nested lists) is handled by the walk; 7 callbacks touch no D3D; 8 nop
    }
}

// the walk: a stack of lists of 8-byte entries {key, packet}; type 6 packets push {+8 entries, +0xc count}
static void replay(uint32_t sr) {
    uint32_t i = rd32(sr + 0x33df0); if (rd8(sr + 0x25)) i ^= 1;
    uint32_t base = rd32(sr + 0x433e08 + 4 * i), n = rd32(sr + 0x433e10 + 4 * i);
    struct { uint32_t cur, end; } st[32]; int sp = 0;
    st[0].cur = base; st[0].end = base + 8 * n;
    while (sp >= 0) {
        if (st[sp].cur >= st[sp].end) { sp--; continue; }
        uint32_t pk = rd32(st[sp].cur + 4); st[sp].cur += 8;
        if (!pk) continue;
        uint32_t type = rd16(pk + 2) & 0xF;
        if (type == 6) {
            if (sp + 1 < 32) { sp++; st[sp].cur = rd32(pk + 8); st[sp].end = st[sp].cur + 8 * rd32(pk + 0xc); }
            continue;
        }
        rn_stat[7]++;
        packet(pk);
    }
}

// F_7bc8a0: sRender::replay (thiscall)
double rn_replay_ms;
void N_replay(CPU *c) {
    uint32_t sr = c->ecx;
    if (mtlr_shadow()) CALL_ORIG(7bc8a0);
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    {   // look-ahead pass: start every pipeline this frame needs in parallel, then replay for real
        int stat[8], t_w = tw, t_h = th, so = scissor_on, sc[4], tr = rn_trace;
        memcpy(stat, rn_stat, sizeof stat); memcpy(sc, scissor_rect, sizeof sc); rn_trace = 0;
        mtlr_lookahead_begin();
        replay(sr);
        mtlr_lookahead_end();
        memcpy(rn_stat, stat, sizeof stat); memcpy(scissor_rect, sc, sizeof sc); tw = t_w; th = t_h; scissor_on = so; rn_trace = tr;
    }
    replay(sr);
    mtlr_drain_pool();   // this thread's autoreleased Metal objects (encoders, descriptors) of the frame
    clock_gettime(CLOCK_MONOTONIC, &b);
    rn_replay_ms += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

// ---------------- shader packages ----------------
// F_b77d30 loads a shader package: creates its vertex and pixel shaders, then fills its program table (pkg = this+0x8c,
// programs at pkg+0x401c, 36 bytes each, laid out as apply_shader reads them; count: the 16-bit field at this+0x72).
// Every program's pipeline is started in the background right away, against the most used target/blend setups, so
// it is usually ready by its first draw (Metal compiles a pipeline's shaders when it is built: 10-120 ms each).
static int spec_top(void) { static int n = -1; if (n < 0) { const char *e = getenv("RT_RN_SPEC"); n = e ? atoi(e) : 2; } return n; }
void N_pkg_load(CPU *c) {   // thiscall, 1 stack argument
    uint32_t self = c->ecx;
    CALL_ORIG(b77d30);
    uint32_t pkg = rd32(self + 0x8c), n = rd16(self + 0x72);
    int before = mtlr_speculated();
    // breadth first: every program's most likely setup, then the next one, so the likely ones are ready first
    if (pkg) for (int r = 0; r < spec_top(); r++) for (uint32_t i = 0; i < n; i++) {
        uint32_t P = pkg + 0x401c + 36 * i;
        RnObj *vs = rn_get(rd32(P + 0x10)), *ps = rn_get(rd32(P + 0x14)), *dl = rn_get(rd32(P + 0x18));
        if (vs && ps && vs->kind == RN_VS && ps->kind == RN_PS) mtlr_speculate(vs->hash, ps->hash, dl && dl->kind == RN_DECL ? dl->layout : 0, r);
    }
    if (n) rt_log("[rn] shader package %08x: %u programs, %d pipelines started", self, n, mtlr_speculated() - before);
    NATIVE_POP(1);
}
