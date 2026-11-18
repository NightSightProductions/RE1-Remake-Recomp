// Backend setup: the converted shaders and the output.
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "rn.h"

static const Sm3ShaderInfo shader_table[] = {
#include "sm3_shaders.inc"
};

static int ready;
static void setup(void) {
    if (ready) return;
    ready = 1;
    // the shaders: a metallib (built with Apple's Metal Toolchain) or Metal source batches (shaders-msl, compiled by
    // macOS at startup); in the .app (Resources/shaders.metallib or Resources/shaders_msl), else in the build folder.
    // RT_SHADERS_MSL=1 prefers the sources.
    char p[1200];
    const char *metallib[] = {"%s/../Resources/shaders.metallib", "%s/shaders/shaders.metallib", "%s/../shaders/shaders.metallib"};
    const char *msl[] = {"%s/../Resources/shaders_msl", "%s/shaders/msl", "%s/../shaders/msl"};
    int found = 0, want_src = getenv("RT_SHADERS_MSL") != NULL;
    for (int pass = 0; pass < 2 && !found; pass++) {
        const char **list = (pass == 0) != want_src ? metallib : msl;
        for (int i = 0; i < 3 && !found; i++) { snprintf(p, sizeof p, list[i], rt_exe_dir); found = !access(p, R_OK); }
    }
    if (!found) snprintf(p, sizeof p, "build/shaders/shaders.metallib");
    mtlr_load_shaders(p, shader_table, (int)(sizeof shader_table / sizeof *shader_table));
    mtlr_prewarm();
    {   // the target/blend setups most pipelines use (from a playthrough; the depth format doesn't matter): BGRA8
        // opaque, alpha blended (two alpha-channel variants), R32F (shadow/depth passes), RGB-only writes, additive
        // over alpha; the backend keeps counting the ones it meets
        static const MtlBlend opaque = {0, 1, 0, 0, 1, 0, 0, 15}, alpha = {1, 4, 5, 0, 1, 0, 0, 15}, alpha2 = {1, 4, 5, 0, 0, 5, 0, 15},
                              rgb = {0, 1, 0, 0, 1, 0, 0, 14}, add = {1, 4, 1, 0, 0, 1, 0, 15}, one = {1, 1, 0, 0, 0, 0, 0, 15},
                              depth_only = {0, 1, 0, 0, 1, 0, 0, 0}, none = {0};
        const uint32_t bgra[4] = {80}, r32f[4] = {55}, rgba16f[4] = {115};
        mtlr_speculate_config(bgra, 260, 0, (MtlBlend[4]){opaque, none, none, none}, 189);
        mtlr_speculate_config(bgra, 260, 0, (MtlBlend[4]){alpha, none, none, none}, 38);
        mtlr_speculate_config(r32f, 260, 0, (MtlBlend[4]){opaque, none, none, none}, 23);
        mtlr_speculate_config(bgra, 260, 0, (MtlBlend[4]){alpha2, none, none, none}, 18);
        mtlr_speculate_config(bgra, 260, 0, (MtlBlend[4]){rgb, none, none, none}, 12);
        mtlr_speculate_config(bgra, 260, 0, (MtlBlend[4]){add, none, none, none}, 8);
        mtlr_speculate_config(rgba16f, 260, 0, (MtlBlend[4]){one, none, none, none}, 5);      // RGBA16F, blending on (One/Zero)
        mtlr_speculate_config(bgra, 260, 0, (MtlBlend[4]){depth_only, none, none, none}, 3);   // no color writes
    }
}
void rn_setup(void) { setup(); }
