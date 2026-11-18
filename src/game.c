// Resident Evil HD Remaster (bhd.exe, Steam app 304240) on Recomp-Framework's win32 target.
//
// usage: re1 [game folder | bhd.exe] [--image orig.exe] [-- game arguments]
//        re1 --compile-shaders   compile the shaders once (Metal source builds: the installer runs it), then exit
//   game folder: the folder with bhd.exe and nativePC (or bhd.exe in it, as Steam passes it through NSR) (default: the Steam for Mac library, RE1_GAME_DIR, or
//                ~/Library/Application Support/re1-recomp/game-dir.txt)
//   --image:     the DRM-free executable made at build time (tools/unwrap_steamstub32.py); by default orig.exe
//                in the app's Resources or next to this binary (rt_game.image)
#include "rt32.h"
#include "mtl_render.h"
#include <unistd.h>
#include <mach-o/dyld.h>
#include <strings.h>
#include <sys/stat.h>

void rt_init(const char *image_path);
void rt_run(void);
extern char rt_cmdline[1024];

static int is_game_dir(const char *d) {
    char p[1200]; struct stat st;
    snprintf(p, sizeof p, "%s/nativePC", d);
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// F_891100, the main thread's message loop, runs Steam's callbacks and loops again at once whenever no window message
// is waiting: a whole core at 100% for nothing (the game and render threads are separate). After those callbacks
// (the call at 891164, patches.txt) it now sleeps 1 ms.
void I_steam_api_SteamAPI_RunCallbacks(CPU *c);
void N_msgloop_idle(CPU *c) { I_steam_api_SteamAPI_RunCallbacks(c); usleep(1000); }

int main(int argc, char **argv) {
    const char *home = getenv("HOME");
    const char *image = NULL;
    rt_verbose = getenv("RT_VERBOSE") != NULL;
    int compile_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--compile-shaders")) compile_only = 1;
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) image = argv[++i];
        else if (!strcmp(argv[i], "--")) { break; }
        else if (argv[i][0] != '-' && !rt_game_dir[0]) {
            // the game folder, or the game's exe in it (Steam through NSR: `re1 <bhd.exe path> <game folder> [args]`);
            // anything else (game arguments Steam passes along) is ignored
            char d[1024]; snprintf(d, sizeof d, "%s", argv[i]);
            size_t l = strlen(d);
            if (l > 4 && !strcasecmp(d + l - 4, ".exe")) { char *sl = strrchr(d, '/'); if (sl) *sl = 0; }
            if (is_game_dir(d)) snprintf(rt_game_dir, sizeof rt_game_dir, "%s", d);
        }
    }
    if (compile_only) {   // the installer: compile the shaders once (macOS keeps them for this app), then exit
        char self[1024]; uint32_t sz = sizeof self;
        if (!_NSGetExecutablePath(self, &sz)) { char *sl = strrchr(self, '/'); if (sl) *sl = 0; snprintf(rt_exe_dir, sizeof rt_exe_dir, "%s", self); }
        snprintf(rt_data_dir, sizeof rt_data_dir, "%s/Library/Application Support/re1-recomp", home);
        if (!mtlr_init_shaders_only()) { fprintf(stderr, "re1: no Metal device\n"); return 1; }
        void rn_setup(void); rn_setup();
        mtlr_wait_shaders();
        printf("shaders ready\n");
        return 0;
    }
    rt_log_to_file("RE1-recomp");   // from Finder or Steam: ~/Library/Logs/RE1-recomp.log
    if (!rt_game_dir[0] && getenv("RE1_GAME_DIR")) snprintf(rt_game_dir, sizeof rt_game_dir, "%s", getenv("RE1_GAME_DIR"));
    if (!rt_game_dir[0]) {
        char p[1024]; snprintf(p, sizeof p, "%s/Library/Application Support/re1-recomp/game-dir.txt", home);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(rt_game_dir, sizeof rt_game_dir, f)) rt_game_dir[strcspn(rt_game_dir, "\r\n")] = 0; fclose(f); }
    }
    if (!rt_game_dir[0])
        snprintf(rt_game_dir, sizeof rt_game_dir, "%s/Library/Application Support/Steam/steamapps/common/Resident Evil Biohazard HD REMASTER", home);
    if (!is_game_dir(rt_game_dir)) { fprintf(stderr, "re1: no game data in %s (pass the folder that contains nativePC)\n", rt_game_dir); return 1; }

    char abs_img[1200];   // an explicit --image, relative to where we were started (we chdir to the game below)
    if (image && image[0] != '/') { char cwd[1024]; getcwd(cwd, sizeof cwd); snprintf(abs_img, sizeof abs_img, "%s/%s", cwd, image); image = abs_img; }

    snprintf(rt_data_dir, sizeof rt_data_dir, "%s/Library/Application Support/re1-recomp", home);
    mkdir(rt_data_dir, 0755);
    rt_game = (RtGame32){"Resident Evil HD Remaster", "bhd.exe", "C:\\Games\\RE1", "C:\\Users\\Player\\Documents", "orig.exe", 304240};
    snprintf(rt_cmdline, sizeof rt_cmdline, "\"%s\\%s\"", rt_game.win_dir, rt_game.exe);
    if (chdir(rt_game_dir)) { perror("chdir"); return 1; }
    if (rt_verbose) fprintf(stderr, "[re1] game %s\n", rt_game_dir);
    mtlr_init();          // the native Metal renderer wraps guest memory while rt_init maps it
    rt_init(image);
    rt_run();
    return 0;
}
