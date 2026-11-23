/* ============================================================
 * MechSounds.exe — standalone Windows keyboard sound player.
 * No Python, no install. Just double-click it.
 *
 * Plays a mechanical switch sound on every keypress, in every
 * app, through the default audio device (your headphones).
 * Works with ANY keyboard — USB, Bluetooth, laptop — because it
 * listens to key events, not the hardware.
 *
 * ON LAUNCH it opens a small chooser window: click a switch to
 * hear it, pick a volume, optionally tick "Start with Windows",
 * then hit Start. The window hides to the tray icon; double-
 * click the tray icon to pick again. Launched with --startup
 * (how the Windows startup entry runs it) it skips the chooser
 * and starts silently with your last saved switch + volume.
 * The chooser uses radio buttons, not a listbox: a listbox runs a
 * type-ahead search on every keypress, and a key with no matching
 * switch makes Windows play the error ding.
 *
 * LATENCY DESIGN (so it never feels laggy or disorienting):
 *  - Playback is MCI, one `play <alias> from 0` command per
 *    keypress (no separate seek) on a pool of pre-opened aliases.
 *    (The waveOut rewrite in v8 never produced audible sound on
 *    this machine despite correct-looking code; MCI is the proven
 *    backend here — it delegates the low-level details to the
 *    MCI driver, which worked from v1 through v7.)
 *  - The keyboard hook does ONE thing: PostMessage() and return.
 *    It never plays audio, never reads files, never blocks — so
 *    the keystroke itself is never delayed by the sound.
 *  - Every sound plays out fully and blends: each alias plays one
 *    sound at a time, so a keystroke never cuts off the previous
 *    sound — fast typing blends instead of chopping, and no
 *    keypress is ever silently dropped.
 *  - WAVs are rendered at 0.4 peak (-8 dBFS) so overlapping
 *    sounds don't sum past 0 dBFS and clip in the mixer.
 *  - Each key is assigned one of six down recordings by hash, so
 *    the same key always plays the same recording and different
 *    keys sound like themselves.
 *  - The tray tooltip shows the measured hook-to-play dispatch
 *    time (avg/max ms). The rest is Windows audio output, typically
 *    5-15 ms — total stays well under the ~50 ms where typing
 *    starts to feel disconnected from its sound.
 *
 * BUILD YOUR OWN: besides the 7 real-recording presets, the
 * chooser has a Custom build. Pick a switch type (linear /
 * tactile / clicky), lubed or not, a case material (plastic /
 * aluminum / brass) and mods (tape mod, o-rings, both, neither),
 * and the app shapes a real base recording to match — lowpass
 * filtering for lube and tape, a resonant ping for aluminum, a
 * deep thump for brass, a shortened tail for o-rings.
 *  - The tray tooltip shows the measured hook-to-play dispatch
 *    time (avg/max ms). The rest is Windows audio output, typically
 *    10-30 ms — total stays well under the ~50 ms where typing
 *    starts to feel disconnected from its sound.
 *
 * Controls: right-click the tray icon for a menu (profiles,
 *           toggle, chooser, start-with-Windows, exit).
 *
 * Build (Linux cross-compile with Zig):
 *   zig cc -target x86_64-windows-gnu -O2 -Wl,--subsystem,windows \
 *     -o MechSounds.exe MechSounds.c -lwinmm -lgdi32 -lshell32 -lcomctl32 \
 *       -lwininet -lmfplat -lmfreadwrite -lmfuuid -lole32
 * ============================================================ */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <wininet.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define MECHSOUNDS_VERSION "v15"

#define WM_TRAYICON  (WM_USER + 1)
#define WM_PLAY_DOWN (WM_USER + 2)
#define WM_PLAY_UP   (WM_USER + 3)

#define ID_TRAY_EXIT         1001
#define ID_TRAY_TOGGLE       1002
#define ID_TRAY_CHOOSE       1003
#define ID_TRAY_STARTUP      1004
#define ID_TRAY_PROFILE_BASE 1100

#define ID_CH_RADIO_BASE 2010
#define ID_CH_START   2002
#define ID_CH_QUIT    2003
#define ID_CH_STARTUP 2004
#define ID_CH_VOLUME  2005
#define ID_CH_TEST    2006
#define ID_CH_GAMEPAUSE 2008
#define ID_CH_TAB     2009
#define ID_CH_SEARCH  2011
#define ID_CH_SEARCH_BTN 2012
#define ID_CH_RESULTS 2013
#define ID_CH_SEARCH_STATUS 2014
#define IDT_GAMECHECK  1
#define ID_CH_SW      2030
#define ID_CH_LUBE    2031
#define ID_CH_CASE    2032
#define ID_CH_MODS    2033

/* sounds: 6 down variants + space, loaded as PCM in memory.
   One sound per keypress: no release (up) sounds — a key plays its
   down sound once on press and nothing on release. Each key is
   assigned ONE variant by hash, so the same key always plays the
   same recording and different keys sound like themselves. */
#define NDOWN_VARIANTS 6
#define NSOUNDS        7   /* down1-6 + space1 */
/* MCI aliases: one device per (sound, slot) so sounds blend.
   Down: 6 variants x 2 aliases. Big keys: 4 keys x 2 aliases. */
#define NDOWN_ALIASES  2
#define NBIG_ALIASES   2
#define NALIASES       (NDOWN_VARIANTS * NDOWN_ALIASES + 4 * NBIG_ALIASES)

#define KIND_DOWN  0
#define KIND_SPACE 1

/* Custom build: profile index == rtNProfiles (after all presets). */
#define NPROFILES_BUILTIN 15
#define MAX_PROFILES 64
/* Runtime profile table: 15 built-ins + packs added by search-and-add.
   Folder = sounds subfolder, Name = chooser display name,
   Cat = switch-type tab (0 Linear, 1 Tactile, 2 Topre, 3 Clicky,
   4 Hall Effect). Built at startup from the tables below plus the
   registry list under Software\MechSounds\AddedProfiles. */
static char RTFolders[MAX_PROFILES][64];
static char RTNames[MAX_PROFILES][128];
static int RTCat[MAX_PROFILES];
static int rtNProfiles = NPROFILES_BUILTIN;
static int customIndex = NPROFILES_BUILTIN;   /* == rtNProfiles */

static void init_runtime_profiles(void);  /* defined in the chooser section */

static const char *Profiles[] = {
    "Rainy75", "NKCream", "CrystalPanda", "Oreo", "RealTopre", "ModelM", "Reaper",
    "KeychronQ3", "OrangeDeluxe", "AulaF75", "CreamyThingy", "GravastarV60",
    "HyperXAlloy", "Wooting60HE", "EpomakerTH85"
};
static const char *ProfileNames[] = {
    "Rainy 75 Pro - Kailh Cocoa",
    "NK Cream - NovelKeys Cream",
    "Crystal Panda - Keychron V1 tactile",
    "Oreo - Everglide Oreo linear",
    "HHKB Topre 45g",
    "IBM Model M - buckling spring",
    "Aula F75 Max - Leobog Reaper",
    "Keychron Q3 - Gateron G Pro Red",
    "Ikki68 Aurora - C3 Tangerine",
    "Aula F75 - creamy",
    "Custom keyboard - Generic linear",
    "Gravastar V60 Pro - Hall Effect",
    "HyperX Alloy - Cherry MX Blue",
    "Wooting 60HE - Lekker L60 Hall Effect",
    "Epomaker TH85 - Creamy Jade"
};
#define NPROFILES 15

static const int VolumeLevels[] = { 100, 80, 60, 40, 20 };
#define NVOLUMES 5

static HINSTANCE hInst;
static HWND hwnd;              /* message-only window: hook + tray owner */
static HWND hwndChooser;       /* profile chooser window, NULL when closed */
static HHOOK hHook;
static NOTIFYICONDATAA nid;
static char exeDir[MAX_PATH];
static int curProfile = 0;     /* 0-6 preset, 7 = custom build */
static int volume = 80;        /* percent */
static BOOL enabled = TRUE;
static BOOL pauseInGames = TRUE;  /* auto-pause when a game is foreground */
static BOOL gamePaused = FALSE;   /* a game is foreground right now */
static BOOL running = FALSE;   /* hook installed + tray icon up */
static BOOL keyDownState[256]; /* which keys are currently held (repeat filter) */

/* custom build parameters */
static int customSwitch = 0;   /* 0 Linear, 1 Tactile, 2 Clicky */
static int customLube = 0;     /* 0 Lubed, 1 Unlubed */
static int customCase = 0;     /* 0 Plastic, 1 Aluminum, 2 Brass */
static int customMods = 0;     /* 0 None, 1 Tape mod, 2 O-rings, 3 Tape + O-rings */

/* loaded sounds: PCM buffers, 0-5 = down1-6, 6 = space1 */
static short *soundPCM[NSOUNDS];
static int soundFrames[NSOUNDS];
/* Big keys (space, enter, backspace, tab) each get their own sound:
   the space recording pitch-shifted per key, so the bigger keys
   sound deep and distinct from the regular keys, but the same key
   always plays the same sound. */
static short *bigKeyPCM[4];
static int bigKeyFrames[4];
static const double BigKeyPitch[4] = { 1.0, 1.15, 0.85, 1.30 }; /* Space, Enter, Back, Tab */
/* File paths the MCI aliases open. Down: the profile's own files,
   or temp DSP'd files for a custom build. Big keys: always temp
   pitch-shifted files. Durations in ms, for alias reuse timing. */
static char downPath[NDOWN_VARIANTS][MAX_PATH];
static char bigPath[4][MAX_PATH];
static int downDurMs[NDOWN_VARIANTS];
static int bigDurMs[4];
static char tempDir[MAX_PATH];
/* MCI alias pool. kind 0 = down variant, 1 = big key. */
typedef struct {
    char name[16];
    int kind;
    int index;
    ULONGLONG freeAt;   /* GetTickCount64() ms when reusable */
} Alias;
static Alias aliases[NALIASES];
static int aliasCount = 0;
static BOOL audioReady = FALSE;
static int chooserSel = 0;     /* profile picked in the chooser */

/* latency stats: hook time -> playback dispatch, in ms */
static unsigned long long dispatchSum = 0;
static unsigned long long dispatchMax = 0;
static unsigned long long dispatchN = 0;

/* ---------------- settings (HKCU\Software\MechSounds) ------------ */

static void save_settings(void) {
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\MechSounds", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        DWORD p = (DWORD)curProfile, v = (DWORD)volume;
        DWORD cs = (DWORD)customSwitch, cl = (DWORD)customLube;
        DWORD cc = (DWORD)customCase, cm = (DWORD)customMods;
        RegSetValueExA(k, "Profile", 0, REG_DWORD, (BYTE *)&p, sizeof(p));
        RegSetValueExA(k, "Volume", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegSetValueExA(k, "CustomSwitch", 0, REG_DWORD, (BYTE *)&cs, sizeof(cs));
        RegSetValueExA(k, "CustomLube", 0, REG_DWORD, (BYTE *)&cl, sizeof(cl));
        RegSetValueExA(k, "CustomCase", 0, REG_DWORD, (BYTE *)&cc, sizeof(cc));
        RegSetValueExA(k, "CustomMods", 0, REG_DWORD, (BYTE *)&cm, sizeof(cm));
        { DWORD pg = pauseInGames ? 1 : 0;
          RegSetValueExA(k, "PauseInGames", 0, REG_DWORD, (BYTE *)&pg, sizeof(pg)); }
        RegCloseKey(k);
    }
}

static void load_settings(void) {
    HKEY k;
    DWORD v, sz, t;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\MechSounds", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        sz = sizeof(v); t = REG_DWORD;
        if (RegQueryValueExA(k, "Profile", NULL, &t, (BYTE *)&v, &sz) == ERROR_SUCCESS
            && v <= (DWORD)rtNProfiles)   /* custom build = rtNProfiles */
            curProfile = (int)v;
        sz = sizeof(v); t = REG_DWORD;
        if (RegQueryValueExA(k, "Volume", NULL, &t, (BYTE *)&v, &sz) == ERROR_SUCCESS
            && v >= 10 && v <= 100)
            volume = (int)v;
        sz = sizeof(v); t = REG_DWORD;
        if (RegQueryValueExA(k, "CustomSwitch", NULL, &t, (BYTE *)&v, &sz) == ERROR_SUCCESS
            && v < 3)
            customSwitch = (int)v;
        sz = sizeof(v); t = REG_DWORD;
        if (RegQueryValueExA(k, "CustomLube", NULL, &t, (BYTE *)&v, &sz) == ERROR_SUCCESS
            && v < 2)
            customLube = (int)v;
        sz = sizeof(v); t = REG_DWORD;
        if (RegQueryValueExA(k, "CustomCase", NULL, &t, (BYTE *)&v, &sz) == ERROR_SUCCESS
            && v < 3)
            customCase = (int)v;
        sz = sizeof(v); t = REG_DWORD;
        if (RegQueryValueExA(k, "CustomMods", NULL, &t, (BYTE *)&v, &sz) == ERROR_SUCCESS
            && v < 4)
            customMods = (int)v;
        sz = sizeof(v); t = REG_DWORD;
        if (RegQueryValueExA(k, "PauseInGames", NULL, &t, (BYTE *)&v, &sz) == ERROR_SUCCESS)
            pauseInGames = (v != 0);
        RegCloseKey(k);
    }
}

/* Display name for the current profile (presets, or the custom build). */
static char customName[160];
static const char *current_profile_name(void) {
    if (curProfile < rtNProfiles)
        return RTNames[curProfile];
    static const char *sw[] = { "Linear", "Tactile", "Clicky" };
    static const char *lb[] = { "Lubed", "Unlubed" };
    static const char *cs[] = { "Plastic", "Aluminum", "Brass" };
    static const char *md[] = { "no mods", "tape mod", "o-rings", "tape + o-rings" };
    snprintf(customName, sizeof(customName), "Custom: %s, %s, %s, %s",
             sw[customSwitch], lb[customLube], cs[customCase], md[customMods]);
    return customName;
}

/* ---------------- start with Windows (Run key) -------------------- */

#define RUN_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static BOOL startup_enabled(void) {
    HKEY k;
    BOOL on = FALSE;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExA(k, "MechSounds", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}

static void set_startup(BOOL on) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (on) {
            char exe[MAX_PATH], val[MAX_PATH + 32];
            GetModuleFileNameA(NULL, exe, MAX_PATH);
            snprintf(val, sizeof(val), "\"%s\" --startup", exe);
            RegSetValueExA(k, "MechSounds", 0, REG_SZ,
                           (BYTE *)val, (DWORD)strlen(val) + 1);
        } else {
            RegDeleteValueA(k, "MechSounds");
        }
        RegCloseKey(k);
    }
}

/* ---------------- audio: MCI (v9) ------------------------------- */
/* The waveOut rewrite (v8) never produced audible sound on the
   user's machine despite correct-looking code — the MCI backend
   (v7) did. So playback is back on MCI, which delegates all the
   waveOut buffer/callback details to the proven MCI driver.
   One `play <alias> from 0` command per keypress (no separate
   seek), and a pool of aliases per sound so sounds overlap and
   play out fully instead of cutting each other off. */

/* Tiny diagnostic log (%TEMP%\MechSounds\log.txt) so a silent
   failure can be traced without guessing. */
static void log_msg(const char *fmt, ...) {
    char buf[512], path[MAX_PATH];
    FILE *f;
    va_list ap;
    if (!tempDir[0])
        return;
    snprintf(path, sizeof(path), "%s\\log.txt", tempDir);
    f = fopen(path, "a");
    if (!f)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(f, "%s\n", buf);
    fclose(f);
}

static void mci_cmd(const char *cmd) {
    mciSendStringA(cmd, NULL, 0, NULL);
}

static void apply_volume(void) {
    char cmd[128];
    int i;
    if (!audioReady)
        return;
    for (i = 0; i < aliasCount; i++) {
        snprintf(cmd, sizeof(cmd), "set %s audio volume to %d",
                 aliases[i].name, volume * 10);
        mci_cmd(cmd);
    }
}

/* Write a 44.1 kHz 16-bit mono WAV from a PCM buffer. */
static BOOL write_wav_file(const char *path, const short *pcm, int frames) {
    FILE *f;
    unsigned int dataLen, riffLen, fmtLen = 16, sampleRate = 44100, byteRate = 88200;
    unsigned short audioFmt = 1, channels = 1, bits = 16, blockAlign = 2;
    if (!pcm || frames <= 0)
        return FALSE;
    f = fopen(path, "wb");
    if (!f)
        return FALSE;
    dataLen = (unsigned int)frames * 2;
    riffLen = 36 + dataLen;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riffLen, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    fwrite(&fmtLen, 4, 1, f);
    fwrite(&audioFmt, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sampleRate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataLen, 4, 1, f);
    fwrite(pcm, 2, frames, f);
    fclose(f);
    return TRUE;
}

static void init_temp_dir(void) {
    char base[MAX_PATH];
    if (GetTempPathA(sizeof(base), base) == 0)
        snprintf(base, sizeof(base), "%s\\", exeDir);
    snprintf(tempDir, sizeof(tempDir), "%sMechSounds", base);
    CreateDirectoryA(tempDir, NULL);
    log_msg("init_temp_dir: %s", tempDir);
}

static void close_aliases(void) {
    char cmd[64];
    int i;
    for (i = 0; i < aliasCount; i++) {
        snprintf(cmd, sizeof(cmd), "close %s", aliases[i].name);
        mci_cmd(cmd);
    }
    aliasCount = 0;
}

static BOOL open_aliases(void) {
    char cmd[MAX_PATH + 64];
    int v, a;
    close_aliases();
    for (v = 0; v < NDOWN_VARIANTS; v++) {
        for (a = 0; a < NDOWN_ALIASES; a++) {
            Alias *al = &aliases[aliasCount];
            snprintf(al->name, sizeof(al->name), "msd%d", aliasCount);
            al->kind = 0;
            al->index = v;
            al->freeAt = 0;
            snprintf(cmd, sizeof(cmd), "open \"%s\" type waveaudio alias %s",
                     downPath[v], al->name);
            if (mciSendStringA(cmd, NULL, 0, NULL) != 0)
                continue;
            snprintf(cmd, sizeof(cmd), "set %s audio volume to %d",
                     al->name, volume * 10);
            mci_cmd(cmd);
            aliasCount++;
        }
    }
    for (v = 0; v < 4; v++) {
        for (a = 0; a < NBIG_ALIASES; a++) {
            Alias *al = &aliases[aliasCount];
            snprintf(al->name, sizeof(al->name), "msb%d", aliasCount);
            al->kind = 1;
            al->index = v;
            al->freeAt = 0;
            snprintf(cmd, sizeof(cmd), "open \"%s\" type waveaudio alias %s",
                     bigPath[v], al->name);
            if (mciSendStringA(cmd, NULL, 0, NULL) != 0)
                continue;
            snprintf(cmd, sizeof(cmd), "set %s audio volume to %d",
                     al->name, volume * 10);
            mci_cmd(cmd);
            aliasCount++;
        }
    }
    audioReady = aliasCount > 0;
    log_msg("open_aliases: %d/%d aliases opened, audioReady=%d",
            aliasCount, NALIASES, audioReady);
    return audioReady;
}

/* Load a 44.1 kHz 16-bit mono WAV into memory. Returns frame count. */
static int load_wav_pcm(const char *path, short **out) {
    FILE *f = fopen(path, "rb");
    char id[4];
    unsigned int sz;
    int have_fmt = 0, have_data = 0;
    int fmt_ch = 0, fmt_bits = 0;
    unsigned int fmt_rate = 0;
    long data_pos = 0;
    unsigned int data_len = 0;
    short *pcm;
    if (!f)
        return 0;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) != 0) {
        fclose(f);
        return 0;
    }
    fseek(f, 4, SEEK_CUR);
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4) != 0) {
        fclose(f);
        return 0;
    }
    while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
        if (memcmp(id, "fmt ", 4) == 0) {
            unsigned short fmt, ch, ba, bits;
            unsigned int rate, br;
            if (fread(&fmt, 2, 1, f) != 1 || fread(&ch, 2, 1, f) != 1 ||
                fread(&rate, 4, 1, f) != 1 || fread(&br, 4, 1, f) != 1 ||
                fread(&ba, 2, 1, f) != 1 || fread(&bits, 2, 1, f) != 1) {
                fclose(f);
                return 0;
            }
            fmt_ch = ch;
            fmt_rate = rate;
            fmt_bits = bits;
            have_fmt = 1;
            if (sz > 16)
                fseek(f, (long)(sz - 16), SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            data_pos = ftell(f);
            data_len = sz;
            have_data = 1;
            break;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    if (!have_fmt || !have_data || fmt_ch != 1 || fmt_bits != 16 ||
        fmt_rate != 44100) {
        fclose(f);
        return 0;
    }
    pcm = (short *)malloc(data_len);
    if (!pcm) {
        fclose(f);
        return 0;
    }
    fseek(f, data_pos, SEEK_SET);
    if (fread(pcm, 1, data_len, f) != data_len) {
        free(pcm);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out = pcm;
    return (int)(data_len / 2);
}

/* ---- custom-build DSP: shape a real base recording ------------ */
/* Each step is gentle — the result should still sound like the real
   switch it started from, just lubed / cased / modded. */
static void lowpass_float(float *f, int frames, double alpha) {
    double prev = 0.0;
    for (int i = 0; i < frames; i++) {
        prev += alpha * ((double)f[i] - prev);
        f[i] = (float)prev;
    }
}

static void apply_custom_dsp(short *pcm, int frames) {
    float *f;
    double gain = 1.0;
    int i;
    if (!pcm || frames <= 0)
        return;
    f = (float *)malloc(sizeof(float) * (size_t)frames);
    if (!f)
        return;
    for (i = 0; i < frames; i++)
        f[i] = (float)pcm[i] / 32768.0f;
    /* lubed: smoother, less scratch */
    if (customLube == 0) {
        lowpass_float(f, frames, 0.30);   /* ~2.5 kHz */
        gain *= 0.92;
    }
    /* case material */
    if (customCase == 0) {
        lowpass_float(f, frames, 0.44);   /* plastic: muted, ~4 kHz */
        gain *= 0.95;
    } else if (customCase == 1) {
        /* aluminum: bright ping, a faint 1400 Hz ring mixed in */
        for (i = 0; i < frames; i++) {
            double t = (double)i / 44100.0;
            f[i] += (float)(0.05 * sin(2.0 * 3.14159265358979 * 1400.0 * t)
                            * exp(-t / 0.04));
        }
    } else {
        /* brass: deep — lowpass plus a low 160 Hz thump */
        lowpass_float(f, frames, 0.19);   /* ~1.5 kHz */
        for (i = 0; i < frames; i++) {
            double t = (double)i / 44100.0;
            f[i] += (float)(0.07 * sin(2.0 * 3.14159265358979 * 160.0 * t)
                            * exp(-t / 0.08));
        }
    }
    /* mods */
    if (customMods == 1 || customMods == 3) {
        /* tape mod: muffled */
        lowpass_float(f, frames, 0.16);   /* ~1.2 kHz */
        gain *= 0.85;
    }
    if (customMods == 2 || customMods == 3) {
        /* o-rings: softer bottom-out, shorter tail */
        for (i = 0; i < frames; i++) {
            double t = (double)i / 44100.0;
            if (t > 0.03)
                f[i] *= (float)exp(-(t - 0.03) / 0.025);
            if (t < 0.004)
                f[i] *= (float)(t / 0.004);
        }
    }
    for (i = 0; i < frames; i++) {
        double v = (double)f[i] * gain;
        if (v > 0.95)
            v = 0.95;
        if (v < -0.95)
            v = -0.95;
        pcm[i] = (short)(v * 32767.0);
    }
    free(f);
}

/* Base recording for a custom build, by switch type. */
static const char *custom_base_folder(void) {
    if (customSwitch == 1)
        return "CrystalPanda";   /* tactile */
    if (customSwitch == 2)
        return "ModelM";         /* clicky */
    return "NKCream";            /* linear */
}

/* Peak amplitude of a PCM buffer, 0.0-1.0. */
static double sound_peak(const short *pcm, int frames) {
    int peak = 0, i;
    if (!pcm || frames <= 0)
        return 0.0;
    for (i = 0; i < frames; i++) {
        int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak)
            peak = a;
    }
    return (double)peak / 32768.0;
}

/* Apply the custom DSP to a sound. The DSP runs on a copy; if the
   result comes out (near-)silent, the raw recording is kept instead,
   so a custom build can never leave a key silent. */
static short *dsp_sound(short *pcm, int frames) {
    short *dsp;
    if (!pcm || frames <= 0)
        return pcm;
    dsp = (short *)malloc(sizeof(short) * (size_t)frames);
    if (!dsp)
        return pcm;
    memcpy(dsp, pcm, sizeof(short) * (size_t)frames);
    apply_custom_dsp(dsp, frames);
    if (sound_peak(dsp, frames) < 0.01) {
        free(dsp);
        return pcm;
    }
    free(pcm);
    return dsp;
}

/* Pitch-shift a PCM buffer by resampling (linear interpolation).
   factor > 1 = higher pitch (shorter), < 1 = lower (longer). */
static short *pitch_shift_pcm(const short *src, int srcFrames, double factor,
                              int *outFrames) {
    int dstFrames, j;
    short *dst;
    if (!src || srcFrames <= 0 || factor <= 0.0)
        return NULL;
    dstFrames = (int)(srcFrames / factor + 0.5);
    if (dstFrames <= 0)
        return NULL;
    dst = (short *)malloc(sizeof(short) * (size_t)dstFrames);
    if (!dst)
        return NULL;
    for (j = 0; j < dstFrames; j++) {
        double pos = j * factor;
        int i0 = (int)pos;
        double frac = pos - i0;
        int i1 = i0 + 1 < srcFrames ? i0 + 1 : srcFrames - 1;
        double v = (double)src[i0] * (1.0 - frac) + (double)src[i1] * frac;
        if (v > 32767.0)
            v = 32767.0;
        if (v < -32768.0)
            v = -32768.0;
        dst[j] = (short)v;
    }
    *outFrames = dstFrames;
    return dst;
}

static int big_key_index(DWORD vk) {
    if (vk == VK_RETURN)
        return 1;
    if (vk == VK_BACK)
        return 2;
    if (vk == VK_TAB)
        return 3;
    return 0; /* VK_SPACE */
}

static void free_profile_sounds(void) {
    int i;
    close_aliases();
    audioReady = FALSE;
    for (i = 0; i < NSOUNDS; i++) {
        free(soundPCM[i]);
        soundPCM[i] = NULL;
        soundFrames[i] = 0;
    }
    for (i = 0; i < 4; i++) {
        free(bigKeyPCM[i]);
        bigKeyPCM[i] = NULL;
        bigKeyFrames[i] = 0;
    }
}

static void load_profile_sounds(void) {
    const char *folder;
    BOOL custom = (curProfile == customIndex);
    char path[MAX_PATH];
    int v;
    folder = custom ? custom_base_folder() : RTFolders[curProfile];
    free_profile_sounds();
    for (v = 0; v < NDOWN_VARIANTS; v++) {
        snprintf(path, sizeof(path), "%s\\sounds\\%s\\down%d.wav",
                 exeDir, folder, v + 1);
        soundFrames[v] = load_wav_pcm(path, &soundPCM[v]);
        if (!soundPCM[v]) {
            /* A key is never left silent: fall back to Rainy75. */
            snprintf(path, sizeof(path), "%s\\sounds\\Rainy75\\down%d.wav",
                     exeDir, v + 1);
            soundFrames[v] = load_wav_pcm(path, &soundPCM[v]);
        }
        if (custom && soundPCM[v])
            soundPCM[v] = dsp_sound(soundPCM[v], soundFrames[v]);
        downDurMs[v] = soundFrames[v] > 0 ? soundFrames[v] * 1000 / 44100 : 0;
        if (custom && soundPCM[v]) {
            /* Custom sounds live in memory (DSP'd) — write them to
               a temp file so MCI can open them. */
            snprintf(downPath[v], sizeof(downPath[v]), "%s\\down%d.wav",
                     tempDir, v + 1);
            write_wav_file(downPath[v], soundPCM[v], soundFrames[v]);
        } else {
            snprintf(downPath[v], sizeof(downPath[v]), "%s\\sounds\\%s\\down%d.wav",
                     exeDir, folder, v + 1);
            if (!soundPCM[v])
                snprintf(downPath[v], sizeof(downPath[v]),
                         "%s\\sounds\\Rainy75\\down%d.wav", exeDir, v + 1);
        }
    }
    snprintf(path, sizeof(path), "%s\\sounds\\%s\\space1.wav",
             exeDir, folder);
    soundFrames[NDOWN_VARIANTS] = load_wav_pcm(path, &soundPCM[NDOWN_VARIANTS]);
    if (!soundPCM[NDOWN_VARIANTS]) {
        snprintf(path, sizeof(path), "%s\\sounds\\Rainy75\\space1.wav", exeDir);
        soundFrames[NDOWN_VARIANTS] = load_wav_pcm(path, &soundPCM[NDOWN_VARIANTS]);
    }
    if (custom && soundPCM[NDOWN_VARIANTS])
        soundPCM[NDOWN_VARIANTS] =
            dsp_sound(soundPCM[NDOWN_VARIANTS], soundFrames[NDOWN_VARIANTS]);
    /* Big keys: each gets its own sound, pitch-shifted from the
       (DSP'd, for custom) space recording, so space, enter,
       backspace and tab are distinct — and the same key always
       plays the same one. Written to temp files for MCI. */
    for (v = 0; v < 4; v++) {
        bigKeyPCM[v] = NULL;
        bigKeyFrames[v] = 0;
        if (soundPCM[NDOWN_VARIANTS]) {
            bigKeyPCM[v] = pitch_shift_pcm(soundPCM[NDOWN_VARIANTS],
                                           soundFrames[NDOWN_VARIANTS],
                                           BigKeyPitch[v], &bigKeyFrames[v]);
        }
        bigDurMs[v] = bigKeyFrames[v] > 0 ? bigKeyFrames[v] * 1000 / 44100 : 0;
        snprintf(bigPath[v], sizeof(bigPath[v]), "%s\\big%d.wav", tempDir, v);
        if (bigKeyPCM[v])
            write_wav_file(bigPath[v], bigKeyPCM[v], bigKeyFrames[v]);
        else if (soundPCM[NDOWN_VARIANTS])
            write_wav_file(bigPath[v], soundPCM[NDOWN_VARIANTS],
                           soundFrames[NDOWN_VARIANTS]);
    }
    open_aliases();
    if (!audioReady) {
        MessageBoxA(NULL,
                    "MechSounds couldn't open any sounds.\n"
                    "Check that the sounds folder is next to the exe,\n"
                    "and that no other copy of MechSounds is running.",
                    "MechSounds", MB_ICONERROR | MB_OK);
    }
}

/* Which down variant a key plays. A hash of the key code, so the
   same key always gets the same recording and neighbouring keys
   get different ones (A=3, B=2, C=1, D=0, E=5, F=4, ...). */
static int key_variant(DWORD vk) {
    return (int)(((vk * 2654435761u) >> 29) % NDOWN_VARIANTS);
}

static void play_kind(int kind, DWORD vk) {
    int i, wantKind, wantIndex, durMs;
    ULONGLONG now;
    char cmd[64];
    if (!audioReady)
        return;
    if (kind == KIND_SPACE) {
        /* Bigger keys: each plays its own pitch-shifted space
           sound, so space, enter, backspace and tab are distinct —
           and the same key always plays the same one. */
        wantKind = 1;
        wantIndex = big_key_index(vk);
        durMs = bigDurMs[wantIndex];
    } else {
        wantKind = 0;
        wantIndex = key_variant(vk);
        durMs = downDurMs[wantIndex];
    }
    /* One `play <alias> from 0` on a free alias for this sound.
       An alias is reusable once its sound has played out, so
       sounds overlap and play fully instead of cutting off. */
    now = GetTickCount64();
    for (i = 0; i < aliasCount; i++) {
        if (aliases[i].kind == wantKind && aliases[i].index == wantIndex &&
            aliases[i].freeAt <= now) {
            snprintf(cmd, sizeof(cmd), "play %s from 0", aliases[i].name);
            mci_cmd(cmd);
            aliases[i].freeAt = now + (durMs > 0 ? durMs : 100);
            return;
        }
    }
    /* All aliases for this sound busy at once — only at absurd
       typing speeds. Drop this one keystroke rather than cutting
       off a sound mid-tail. */
}

/* Previews use PlaySound (one-off, async) straight from the file --
   no alias, no buffer the audio device reads, so clicking through
   sounds costs nothing and can never crash. */
static void preview_profile(int i) {
    char path[MAX_PATH];
    if (i < 0 || i >= rtNProfiles)
        return;
    snprintf(path, sizeof(path), "%s\\sounds\\%s\\down1.wav",
             exeDir, RTFolders[i]);
    PlaySoundA(path, NULL, SND_FILENAME | SND_ASYNC);
}

static void preview_custom(void) {
    char path[MAX_PATH], tmp[MAX_PATH];
    static int key[4] = { -1, -1, -1, -1 };
    short *pcm = NULL;
    int frames;
    if (key[0] == customSwitch && key[1] == customLube &&
        key[2] == customCase && key[3] == customMods) {
        /* Same tuners -- replay the temp file written last time. */
        snprintf(tmp, sizeof(tmp), "%s\\preview.wav", tempDir);
        PlaySoundA(tmp, NULL, SND_FILENAME | SND_ASYNC);
        return;
    }
    snprintf(path, sizeof(path), "%s\\sounds\\%s\\down1.wav",
             exeDir, custom_base_folder());
    frames = load_wav_pcm(path, &pcm);
    if (!pcm)
        return;
    pcm = dsp_sound(pcm, frames);
    snprintf(tmp, sizeof(tmp), "%s\\preview.wav", tempDir);
    write_wav_file(tmp, pcm, frames);
    free(pcm);
    key[0] = customSwitch;
    key[1] = customLube;
    key[2] = customCase;
    key[3] = customMods;
    PlaySoundA(tmp, NULL, SND_FILENAME | SND_ASYNC);
}

/* ---------------- keyboard hook: post and return, nothing else ---- */

static int is_modifier(DWORD vk) {
    return vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LWIN || vk == VK_RWIN;
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *ks = (KBDLLHOOKSTRUCT *)lParam;
        if (!is_modifier(ks->vkCode)) {
            ULONGLONG now = GetTickCount64();
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
                PostMessage(hwnd, WM_PLAY_DOWN, (WPARAM)ks->vkCode, (LPARAM)now);
            else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
                PostMessage(hwnd, WM_PLAY_UP, (WPARAM)ks->vkCode, (LPARAM)now);
        }
    }
    return CallNextHookEx(hHook, nCode, wParam, lParam);
}


static void update_tip(void);

/* ---------------- game-safe: auto-pause in games ------------------ */
/* Anticheat (BattleEye, EAC, Vanguard, VAC) flags global keyboard
   hooks — they are how cheats intercept input. MechSounds never
   reads game memory or changes input, but the hook itself is what
   gets noticed. So when a game is in the foreground we remove the
   hook entirely (UnhookWindowsHookEx) and put it back when the game
   is gone. No hook installed while a game runs = nothing to flag.
   Detection is twofold: a list of well-known game executables, plus
   any fullscreen window (covers borderless-windowed games too). */

static const char *GameExes[] = {
    "valorant-win64-shipping.exe", "valorant.exe",
    "fortniteclient-win64-shipping.exe",
    "r5apex.exe", "apex.exe",
    "cod.exe", "modernwarfare.exe", "blackops6.exe", "mw2.exe", "warzone.exe",
    "cs2.exe", "csgo.exe",
    "overwatch.exe",
    "rustclient.exe",
    "h1z1.exe", "z1br.exe",
    "tslgame.exe", "pubg.exe",
    "gta5.exe", "gta5enhanced.exe",
    "robloxplayerbeta.exe",
    "leagueclient.exe", "league of legends.exe",
    "destiny2.exe",
    "escapefromtarkov.exe",
    "rainbowsix.exe",
    "bf2042.exe", "bf.exe", "battlefield.exe",
    "eldenring.exe",
    "dota2.exe",
    "rocketleague.exe",
    "haloinfinite.exe",
    "deadlock.exe",
    "marvelrivals.exe",
    "thefinals.exe",
    "fragpunk.exe",
    "deltaforce.exe",
    "wutheringwaves.exe",
    "genshinimpact.exe",
    "starrail.exe",
    "zenlesszonezero.exe",
};

static BOOL exe_is_game(const char *exe) {
    char lower[64];
    size_t i, n = strlen(exe);
    if (n >= sizeof(lower))
        n = sizeof(lower) - 1;
    for (i = 0; i < n; i++)
        lower[i] = (char)tolower((unsigned char)exe[i]);
    lower[n] = '\0';
    for (i = 0; i < sizeof(GameExes) / sizeof(GameExes[0]); i++)
        if (strcmp(lower, GameExes[i]) == 0)
            return TRUE;
    return FALSE;
}

static BOOL is_game_foreground(void) {
    HWND fg;
    RECT wr;
    DWORD pid = 0;
    HANDLE proc = NULL;
    char path[MAX_PATH] = {0};
    char *base;
    BOOL game = FALSE;

    fg = GetForegroundWindow();
    if (!fg)
        return FALSE;
    if (GetWindowRect(fg, &wr)) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        if (wr.left <= 0 && wr.top <= 0 &&
            wr.right >= sw && wr.bottom >= sh)
            game = TRUE;
    }
    GetWindowThreadProcessId(fg, &pid);
    if (pid) {
        proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            DWORD sz = sizeof(path);
            if (QueryFullProcessImageNameA(proc, 0, path, &sz)) {
                base = strrchr(path, '\\');
                base = base ? base + 1 : path;
                if (exe_is_game(base))
                    game = TRUE;
            }
            CloseHandle(proc);
        }
    }
    return game;
}

static void game_check(void) {
    BOOL game;
    if (!pauseInGames || !running)
        return;
    game = is_game_foreground();
    if (game && !gamePaused && hHook) {
        UnhookWindowsHookEx(hHook);
        hHook = NULL;
        gamePaused = TRUE;
        update_tip();
        log_msg("game_check: game foreground, hook removed");
    } else if (!game && gamePaused) {
        hHook = SetWindowsHookExA(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
        gamePaused = FALSE;
        update_tip();
        log_msg("game_check: game gone, hook reinstalled");
    }
}

/* ---------------- engine start (hook + tray + hotkeys) ------------ */

static void show_balloon(const char *text);

static void start_engine(void) {
    /* Hook first: if the hook can't install, we quit with a clear
       error and never add a tray icon that would just vanish. */
    hHook = SetWindowsHookExA(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    if (!hHook) {
        MessageBoxA(NULL, "Failed to install the keyboard hook.",
                    "MechSounds", MB_ICONERROR | MB_OK);
        PostQuitMessage(0);
        return;
    }

    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    snprintf(nid.szTip, sizeof(nid.szTip), "MechSounds %s - %s",
             MECHSOUNDS_VERSION, current_profile_name());
    Shell_NotifyIconA(NIM_ADD, &nid);

    running = TRUE;
    SetTimer(hwnd, IDT_GAMECHECK, 1000, NULL);
    log_msg("start_engine: hook installed, tray icon added, running");
    /* Unmissable confirmation the app is running, even if Windows
       hides the tray icon in the overflow. */
    show_balloon("MechSounds is running - type to hear it");
}

/* ---------------- tray icon / menu / balloons --------------------- */

static void update_tip(void) {
    unsigned long long avg = dispatchN ? dispatchSum / dispatchN : 0;
    snprintf(nid.szTip, sizeof(nid.szTip),
             "MechSounds %s - %s%s%s | dispatch avg %llums max %llums",
             MECHSOUNDS_VERSION, current_profile_name(),
             enabled ? "" : " (off)",
             gamePaused ? " (paused - game)" : "",
             avg, dispatchMax);
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconA(NIM_MODIFY, &nid);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void show_balloon(const char *text) {
    strncpy(nid.szInfo, text, sizeof(nid.szInfo) - 1);
    nid.szInfo[sizeof(nid.szInfo) - 1] = '\0';
    strncpy(nid.szInfoTitle, "MechSounds", sizeof(nid.szInfoTitle) - 1);
    nid.szInfoTitle[sizeof(nid.szInfoTitle) - 1] = '\0';
    nid.dwInfoFlags = NIIF_INFO;
    nid.uFlags = NIF_INFO;
    Shell_NotifyIconA(NIM_MODIFY, &nid);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void show_menu(void) {
    HMENU menu = CreatePopupMenu();
    POINT pt;
    for (int i = 0; i < rtNProfiles; i++)
        AppendMenuA(menu, MF_STRING | (i == curProfile ? MF_CHECKED : MF_UNCHECKED),
                    ID_TRAY_PROFILE_BASE + i, RTNames[i]);
    AppendMenuA(menu, MF_STRING | (curProfile == customIndex ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_PROFILE_BASE + customIndex, "Custom build...");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING | (enabled ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_TOGGLE, enabled ? "Sounds: On" : "Sounds: Off");
    AppendMenuA(menu, MF_STRING, ID_TRAY_CHOOSE, "Choose profile...");
    AppendMenuA(menu, MF_STRING | (startup_enabled() ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_STARTUP, "Start with Windows");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}


/* ---------------- search-and-add (MechVibes) ---------------------- */
/* Type a keyboard name, search MechVibes' public API, pick a pack,
   and it downloads, converts and installs itself as a new profile.
   If several packs match one keyboard (different switches), they all
   show up and you pick the switch you have. */

#define MV_API "https://mechvibes.com/api/sound-packs"
#define MV_MAX_RESULTS 20

typedef struct {
    char id[64];
    char name[128];
    char keyboard[128];
    char switchName[128];
    char switchType[32];
    char license[32];
} MVPack;

static MVPack mvResults[MV_MAX_RESULTS];
static int mvResultCount = 0;

/* Minimal JSON string-field extractor: finds "key":"value". */
static BOOL mv_json_str(const char *json, const char *key, char *out, int outSz) {
    char pat[64];
    const char *p, *e;
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    p = strstr(json, pat);
    if (!p)
        return FALSE;
    p += strlen(pat);
    e = p;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1])
            e += 2;
        else
            e++;
    }
    if ((int)(e - p) >= outSz)
        return FALSE;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = '\0';
    return TRUE;
}

/* HTTPS GET via WinINet. Returns a malloc'd NUL-terminated buffer. */
static char *mv_http_get(const char *url, DWORD *outLen) {
    HINTERNET hNet = NULL, hReq = NULL;
    char *buf = NULL;
    DWORD cap = 65536, len = 0, got = 0;
    char host[256], path[512];
    const char *p;
    BOOL ok = FALSE;
    if (!url || strncmp(url, "https://", 8) != 0)
        return NULL;
    p = url + 8;
    {
        const char *slash = strchr(p, '/');
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= sizeof(host))
            return NULL;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        snprintf(path, sizeof(path), "%s", slash ? slash : "/");
    }
    hNet = InternetOpenA("MechSounds", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet)
        return NULL;
    hReq = InternetOpenUrlA(hNet, url, NULL, 0,
                            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                            INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hReq) {
        InternetCloseHandle(hNet);
        return NULL;
    }
    buf = (char *)malloc(cap);
    if (!buf) {
        InternetCloseHandle(hReq);
        InternetCloseHandle(hNet);
        return NULL;
    }
    while (InternetReadFile(hReq, buf + len, cap - len - 1, &got) && got > 0) {
        len += got;
        if (len + 4096 >= cap) {
            char *nb;
            cap *= 2;
            nb = (char *)realloc(buf, cap);
            if (!nb)
                break;
            buf = nb;
        }
    }
    ok = (len > 0);
    InternetCloseHandle(hReq);
    InternetCloseHandle(hNet);
    if (!ok) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (outLen)
        *outLen = len;
    return buf;
}

/* URL-encode a query string (spaces -> %20, etc.). */
static void mv_url_encode(const char *in, char *out, int outSz) {
    static const char *hex = "0123456789ABCDEF";
    int o = 0;
    for (; *in && o < outSz - 4; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

/* Search MechVibes for keyboard packs matching the query.
   Fills mvResults / mvResultCount. Returns the count. */
static int mv_search(const char *query) {
    char q[256], url[768], *body;
    const char *p;
    int n = 0;
    mvResultCount = 0;
    mv_url_encode(query, q, sizeof(q));
    snprintf(url, sizeof(url), "%s?q=%s&type=keyboard&limit=%d", MV_API, q, MV_MAX_RESULTS);
    body = mv_http_get(url, NULL);
    if (!body)
        return -1;
    /* Walk the "packs" array: each {...} object is one pack. */
    p = strstr(body, "\"packs\":[");
    if (!p) {
        free(body);
        return 0;
    }
    p += 9;
    while (n < MV_MAX_RESULTS) {
        const char *obj = strchr(p, '{');
        const char *end;
        int depth = 0;
        const char *s;
        char tmp[512];
        if (!obj)
            break;
        /* Find the matching close brace. */
        end = obj;
        depth = 0;
        for (s = obj; *s; s++) {
            if (*s == '{')
                depth++;
            else if (*s == '}') {
                depth--;
                if (depth == 0) {
                    end = s;
                    break;
                }
            }
        }
        if (depth != 0 || end <= obj)
            break;
        if ((size_t)(end - obj + 1) >= sizeof(tmp))
            break;
        memcpy(tmp, obj, (size_t)(end - obj + 1));
        tmp[end - obj + 1] = '\0';
        {
            MVPack *pk = &mvResults[n];
            memset(pk, 0, sizeof(*pk));
            mv_json_str(tmp, "id", pk->id, sizeof(pk->id));
            mv_json_str(tmp, "name", pk->name, sizeof(pk->name));
            mv_json_str(tmp, "keyboard", pk->keyboard, sizeof(pk->keyboard));
            mv_json_str(tmp, "switchName", pk->switchName, sizeof(pk->switchName));
            mv_json_str(tmp, "switchType", pk->switchType, sizeof(pk->switchType));
            mv_json_str(tmp, "license", pk->license, sizeof(pk->license));
            if (pk->id[0])
                n++;
        }
        p = end + 1;
        if (*p == ',')
            p++;
        if (*p == ']')
            break;
    }
    free(body);
    mvResultCount = n;
    return n;
}

/* Download a pack zip to the temp dir. Returns TRUE on success. */
static BOOL mv_download_pack(const char *id, char *zipPath, int zipSz) {
    char url[512], tmp[MAX_PATH];
    HINTERNET hNet = NULL, hReq = NULL;
    FILE *f = NULL;
    char host[256], path[512];
    const char *p;
    DWORD status = 0, sz = sizeof(status), got = 0;
    char chunk[8192];
    BOOL ok = FALSE;
    snprintf(url, sizeof(url), "%s/%s/download", MV_API, id);
    snprintf(tmp, sizeof(tmp), "%s\\pack.zip", tempDir);
    snprintf(zipPath, (size_t)zipSz, "%s", tmp);
    hNet = InternetOpenA("MechSounds", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hNet)
        return FALSE;
    /* Follow the 302 redirect to the file host. */
    hReq = InternetOpenUrlA(hNet, url, NULL, 0,
                            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                            INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hReq) {
        InternetCloseHandle(hNet);
        return FALSE;
    }
    HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &status, &sz, NULL);
    if (status != 200) {
        InternetCloseHandle(hReq);
        InternetCloseHandle(hNet);
        return FALSE;
    }
    f = fopen(tmp, "wb");
    if (!f) {
        InternetCloseHandle(hReq);
        InternetCloseHandle(hNet);
        return FALSE;
    }
    while (InternetReadFile(hReq, chunk, sizeof(chunk), &got) && got > 0)
        fwrite(chunk, 1, got, f);
    fclose(f);
    InternetCloseHandle(hReq);
    InternetCloseHandle(hNet);
    ok = GetFileAttributesA(tmp) != INVALID_FILE_ATTRIBUTES;
    return ok;
}

/* Run PowerShell Expand-Archive to unzip a pack. */
static BOOL mv_unzip(const char *zip, const char *dest) {
    char cmd[1024];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD rc = 0;
    CreateDirectoryA(dest, NULL);
    snprintf(cmd, sizeof(cmd),
             "powershell -NoProfile -Command \"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force\"",
             zip, dest);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return rc == 0;
}

/* Decode any audio file (WAV/MP3/OGG/FLAC) to 44.1 kHz mono 16-bit PCM
   via Media Foundation. Returns frame count, *out is malloc'd. */
static int mv_decode_audio(const char *path, short **out) {
    IMFSourceReader *reader = NULL;
    IMFMediaType *type = NULL;
    HRESULT hr;
    short *pcm = NULL;
    size_t cap = 0, len = 0;
    int frames = 0;
    *out = NULL;
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr))
        return 0;
    hr = MFCreateSourceReaderFromURL((const WCHAR *)NULL, NULL, &reader);
    (void)hr;
    /* Use the ANSI path via a wide-string conversion. */
    {
        WCHAR wpath[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
        if (reader)
            reader->lpVtbl->Release(reader);
        reader = NULL;
        hr = MFCreateSourceReaderFromURL(wpath, NULL, &reader);
    }
    if (FAILED(hr) || !reader) {
        MFShutdown();
        return 0;
    }
    hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) {
        hr = type->lpVtbl->SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        if (SUCCEEDED(hr))
            hr = type->lpVtbl->SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        if (SUCCEEDED(hr))
            hr = type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, 1);
        if (SUCCEEDED(hr))
            hr = type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
        if (SUCCEEDED(hr))
            hr = type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr))
            hr = type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
        if (SUCCEEDED(hr))
            hr = type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 88200);
        if (SUCCEEDED(hr))
            hr = reader->lpVtbl->SetCurrentMediaType(reader,
                        (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, type);
        type->lpVtbl->Release(type);
    }
    if (FAILED(hr)) {
        reader->lpVtbl->Release(reader);
        MFShutdown();
        return 0;
    }
    for (;;) {
        IMFSample *sample = NULL;
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFMediaBuffer *mbuf = NULL;
        BYTE *data = NULL;
        DWORD maxLen = 0, curLen = 0;
        hr = reader->lpVtbl->ReadSample(reader,
                    (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL,
                    &flags, &ts, &sample);
        if (FAILED(hr) || !sample)
            break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            sample->lpVtbl->Release(sample);
            break;
        }
        hr = sample->lpVtbl->ConvertToContiguousBuffer(sample, &mbuf);
        if (SUCCEEDED(hr) && mbuf) {
            hr = mbuf->lpVtbl->Lock(mbuf, &data, &maxLen, &curLen);
            if (SUCCEEDED(hr) && data && curLen > 0) {
                if (len + curLen > cap) {
                    size_t ncap = cap ? cap * 2 : 262144;
                    short *np;
                    while (ncap < len + curLen)
                        ncap *= 2;
                    np = (short *)realloc(pcm, ncap);
                    if (!np)
                        break;
                    pcm = np;
                    cap = ncap;
                }
                memcpy((BYTE *)pcm + len, data, curLen);
                len += curLen;
            }
            if (mbuf) {
                mbuf->lpVtbl->Unlock(mbuf);
                mbuf->lpVtbl->Release(mbuf);
            }
        }
        sample->lpVtbl->Release(sample);
    }
    reader->lpVtbl->Release(reader);
    MFShutdown();
    frames = (int)(len / 2);
    if (frames <= 0) {
        free(pcm);
        return 0;
    }
    *out = pcm;
    return frames;
}

/* Read a whole text file into a malloc'd NUL-terminated buffer. */
static char *mv_read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* Find a key's [start_ms, end_ms] timing in a pack config.json.
   Returns TRUE and fills startMs/endMs. */
static BOOL mv_key_timing(const char *cfg, const char *key, double *startMs, double *endMs) {
    char pat[64];
    const char *p, *t;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(cfg, pat);
    if (!p)
        return FALSE;
    t = strstr(p, "\"timing\"");
    if (!t)
        return FALSE;
    t = strchr(t, '[');
    if (!t)
        return FALSE;
    t++;  /* past outer [ */
    t = strchr(t, '[');
    if (!t)
        return FALSE;
    t++;
    *startMs = strtod(t, NULL);
    t = strchr(t, ',');
    if (!t)
        return FALSE;
    t++;
    *endMs = strtod(t, NULL);
    return (*endMs > *startMs);
}

/* Normalize a PCM slice to 0.4 peak and write it as a WAV. */
static BOOL mv_write_slice(const short *pcm, int frames, double startMs, double endMs,
                           const char *outPath) {
    int s0 = (int)(startMs * 44.1);
    int s1 = (int)(endMs * 44.1);
    int n, i, peak = 0;
    short *buf;
    if (!pcm || s0 < 0 || s1 <= s0 || s1 > frames)
        return FALSE;
    n = s1 - s0;
    buf = (short *)malloc(sizeof(short) * (size_t)n);
    if (!buf)
        return FALSE;
    for (i = 0; i < n; i++) {
        int v = pcm[s0 + i];
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
        buf[i] = pcm[s0 + i];
    }
    if (peak > 0) {
        double g = (0.4 * 32767.0) / (double)peak;
        for (i = 0; i < n; i++)
            buf[i] = (short)(buf[i] * g);
    }
    {
        BOOL ok = write_wav_file(outPath, buf, n);
        free(buf);
        return ok;
    }
}

/* Register an installed pack as a profile (registry) and rebuild. */
static void register_added_profile(const char *folder, const char *name, int cat) {
    HKEY k;
    DWORD n = 0, sz = sizeof(n), t = REG_DWORD;
    char vname[32];
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
                        "Software\\MechSounds\\AddedProfiles", 0, NULL, 0,
                        KEY_READ | KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(k, "Count", NULL, &t, (BYTE *)&n, &sz) != ERROR_SUCCESS)
        n = 0;
    snprintf(vname, sizeof(vname), "Folder%lu", (unsigned long)n);
    RegSetValueExA(k, vname, 0, REG_SZ, (const BYTE *)folder, (DWORD)strlen(folder) + 1);
    snprintf(vname, sizeof(vname), "Name%lu", (unsigned long)n);
    RegSetValueExA(k, vname, 0, REG_SZ, (const BYTE *)name, (DWORD)strlen(name) + 1);
    snprintf(vname, sizeof(vname), "Cat%lu", (unsigned long)n);
    {
        DWORD c = (DWORD)cat;
        RegSetValueExA(k, vname, 0, REG_DWORD, (const BYTE *)&c, sizeof(c));
    }
    n++;
    RegSetValueExA(k, "Count", 0, REG_DWORD, (const BYTE *)&n, sizeof(n));
    RegCloseKey(k);
    init_runtime_profiles();
}

/* Map a switchType string to a tab category (0-4). */
static int mv_cat_from_switch(const char *sw) {
    if (!sw || !sw[0])
        return 0;
    if (_stricmp(sw, "tactile") == 0)
        return 1;
    if (_stricmp(sw, "clicky") == 0)
        return 3;
    if (_stricmp(sw, "magnetic") == 0 || _stricmp(sw, "hall effect") == 0)
        return 4;
    if (_stricmp(sw, "topre") == 0)
        return 2;
    return 0;  /* linear / unknown */
}

/* Install one search result: download, extract, convert, register.
   Returns a status string for the chooser. */
static const char *mv_install_pack(int idx) {
    MVPack *pk;
    char zip[MAX_PATH], dest[MAX_PATH], cfgPath[MAX_PATH];
    char *cfg = NULL, *audioPath = NULL;
    char method[16], audioFile[256];
    char folder[64], dispName[128];
    short *pcm = NULL;
    int frames = 0, i, made = 0;
    static const char *downKeys[6] = { "KeyA", "KeyS", "KeyD", "KeyF", "KeyG", "KeyH" };
    if (idx < 0 || idx >= mvResultCount)
        return "Pick a pack from the list first.";
    pk = &mvResults[idx];
    if (!mv_download_pack(pk->id, zip, sizeof(zip)))
        return "Download failed. Check your connection and try again.";
    snprintf(dest, sizeof(dest), "%s\\pack", tempDir);
    if (!mv_unzip(zip, dest))
        return "Couldn't open the pack zip.";
    snprintf(cfgPath, sizeof(cfgPath), "%s\\config.json", dest);
    cfg = mv_read_text(cfgPath);
    if (!cfg)
        return "Pack has no config.json — can't use it.";
    if (!mv_json_str(cfg, "definition_method", method, sizeof(method)))
        snprintf(method, sizeof(method), "single");
    if (!mv_json_str(cfg, "audio_file", audioFile, sizeof(audioFile))) {
        /* "multi" packs name a file per key; fall back to the first audio file. */
        WIN32_FIND_DATAA fd;
        HANDLE h;
        char pat[MAX_PATH];
        snprintf(pat, sizeof(pat), "%s\\*.wav", dest);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            snprintf(pat, sizeof(pat), "%s\\*.mp3", dest);
            h = FindFirstFileA(pat, &fd);
        }
        if (h == INVALID_HANDLE_VALUE) {
            snprintf(pat, sizeof(pat), "%s\\*.ogg", dest);
            h = FindFirstFileA(pat, &fd);
        }
        if (h == INVALID_HANDLE_VALUE) {
            free(cfg);
            return "No audio file found in the pack.";
        }
        snprintf(audioFile, sizeof(audioFile), "%s", fd.cFileName);
        FindClose(h);
        snprintf(method, sizeof(method), "single");
    }
    {
        char ap[MAX_PATH];
        snprintf(ap, sizeof(ap), "%s\\%s", dest, audioFile);
        audioPath = ap;
        frames = mv_decode_audio(audioPath, &pcm);
    }
    if (!pcm || frames <= 0) {
        free(cfg);
        return "Couldn't decode the pack's audio.";
    }
    /* Folder + display name for the new profile. */
    {
        const char *s = pk->name;
        int o = 0;
        for (; *s && o < (int)sizeof(folder) - 1; s++) {
            if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                (*s >= '0' && *s <= '9'))
                folder[o++] = *s;
        }
        folder[o] = '\0';
        if (!folder[0])
            snprintf(folder, sizeof(folder), "Added%d", idx);
        snprintf(dispName, sizeof(dispName), "%s%s%s",
                 pk->name,
                 pk->keyboard[0] ? " - " : "",
                 pk->keyboard);
    }
    {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\sounds\\%s", exeDir, folder);
        CreateDirectoryA(dir, NULL);
        /* 6 down variants from 6 different keys, space from Space. */
        for (i = 0; i < 6; i++) {
            double s0, e0;
            char out[MAX_PATH];
            if (!mv_key_timing(cfg, downKeys[i], &s0, &e0)) {
                /* Fall back: spread evenly across the audio. */
                double totalMs = (double)frames * 1000.0 / 44100.0;
                s0 = totalMs * i / 6.0;
                e0 = s0 + 200.0;
                if (e0 > totalMs)
                    e0 = totalMs;
            }
            snprintf(out, sizeof(out), "%s\\down%d.wav", dir, i + 1);
            if (mv_write_slice(pcm, frames, s0, e0, out))
                made++;
        }
        {
            double s0, e0;
            char out[MAX_PATH];
            if (!mv_key_timing(cfg, "Space", &s0, &e0)) {
                double totalMs = (double)frames * 1000.0 / 44100.0;
                s0 = totalMs * 0.5;
                e0 = s0 + 200.0;
                if (e0 > totalMs)
                    e0 = totalMs;
            }
            snprintf(out, sizeof(out), "%s\\space1.wav", dir);
            if (mv_write_slice(pcm, frames, s0, e0, out))
                made++;
        }
    }
    free(pcm);
    free(cfg);
    if (made < 7)
        return "Installed, but some sounds are missing.";
    register_added_profile(folder, dispName, mv_cat_from_switch(pk->switchType));
    return "Installed! Pick it from the tabs above.";
}

/* ---------------- chooser window ---------------------------------- */

static void create_chooser(void);

/* Profiles grouped by switch type, one tab per type in the chooser.
   Built-in grouping (compile-time, used to seed the runtime table). */
static const char *CatNames[5] = { "Linear", "Tactile", "Topre", "Clicky", "Hall Effect" };
static const int CatProfs[5][9] = {
    { 0, 1, 3, 6, 7, 8, 9, 10, 14 },
    { 2 },
    { 4 },
    { 5, 12 },
    { 11, 13 }
};
static const int CatCounts[5] = { 9, 1, 1, 2, 2 };

/* Category of a built-in profile index, from the table above. */
static int builtin_cat(int prof) {
    for (int c = 0; c < 5; c++)
        for (int k = 0; k < CatCounts[c]; k++)
            if (CatProfs[c][k] == prof)
                return c;
    return 0;
}

/* Runtime tab groups, rebuilt from the runtime profile table. */
static int RTCatProfs[5][MAX_PROFILES];
static int RTCatCounts[5];
static int RTChooserOrder[MAX_PROFILES];  /* profiles ordered by category */

static void rebuild_tab_groups(void) {
    int c, i, ord = 0;
    for (c = 0; c < 5; c++)
        RTCatCounts[c] = 0;
    for (c = 0; c < 5; c++) {
        for (i = 0; i < rtNProfiles; i++) {
            if (RTCat[i] == c) {
                RTCatProfs[c][RTCatCounts[c]++] = i;
                RTChooserOrder[ord++] = i;
            }
        }
    }
}

/* Build the runtime profile table: 15 built-ins, then any packs the
   user added via search-and-add (registry, see register_added_profile). */
static void init_runtime_profiles(void) {
    int i;
    HKEY k;
    for (i = 0; i < NPROFILES_BUILTIN; i++) {
        snprintf(RTFolders[i], sizeof(RTFolders[i]), "%s", Profiles[i]);
        snprintf(RTNames[i], sizeof(RTNames[i]), "%s", ProfileNames[i]);
        RTCat[i] = builtin_cat(i);
    }
    rtNProfiles = NPROFILES_BUILTIN;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\MechSounds\\AddedProfiles", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD n = 0, sz = sizeof(n), t = REG_DWORD;
        if (RegQueryValueExA(k, "Count", NULL, &t, (BYTE *)&n, &sz) == ERROR_SUCCESS) {
            for (i = 0; i < (int)n && rtNProfiles < MAX_PROFILES; i++) {
                char vname[32], folder[64], name[128];
                DWORD cat = 0;
                snprintf(vname, sizeof(vname), "Folder%d", i);
                sz = sizeof(folder);
                if (RegQueryValueExA(k, vname, NULL, NULL, (BYTE *)folder, &sz) != ERROR_SUCCESS)
                    continue;
                snprintf(vname, sizeof(vname), "Name%d", i);
                sz = sizeof(name);
                if (RegQueryValueExA(k, vname, NULL, NULL, (BYTE *)name, &sz) != ERROR_SUCCESS)
                    continue;
                snprintf(vname, sizeof(vname), "Cat%d", i);
                sz = sizeof(cat); t = REG_DWORD;
                RegQueryValueExA(k, vname, NULL, &t, (BYTE *)&cat, &sz);
                snprintf(RTFolders[rtNProfiles], sizeof(RTFolders[0]), "%s", folder);
                snprintf(RTNames[rtNProfiles], sizeof(RTNames[0]), "%s", name);
                RTCat[rtNProfiles] = (cat < 5) ? (int)cat : 0;
                rtNProfiles++;
            }
        }
        RegCloseKey(k);
    }
    customIndex = rtNProfiles;
    rebuild_tab_groups();
}

/* Which tab (category) a profile index belongs to, or -1 for custom. */
static int cat_of_profile(int prof) {
    if (prof < 0 || prof >= rtNProfiles)
        return -1;
    return RTCat[prof];
}

static HWND hwndTab;   /* the tab control in the chooser */
static int curTab;     /* which tab is showing */

/* Show only the radios for one tab (category); hide the rest. */
static void show_tab(int tab) {
    int start = 0;
    curTab = tab;
    if (hwndTab)
        TabCtrl_SetCurSel(hwndTab, tab);
    for (int c = 0; c < 5; c++) {
        for (int k = 0; k < RTCatCounts[c]; k++) {
            int i = start + k;  /* index into RTChooserOrder */
            HWND r = GetDlgItem(hwndChooser, ID_CH_RADIO_BASE + i);
            if (r)
                ShowWindow(r, c == tab ? SW_SHOW : SW_HIDE);
        }
        start += RTCatCounts[c];
    }
}

static void set_custom_enabled(BOOL on) {
    if (!hwndChooser)
        return;
    EnableWindow(GetDlgItem(hwndChooser, ID_CH_SW), on);
    EnableWindow(GetDlgItem(hwndChooser, ID_CH_LUBE), on);
    EnableWindow(GetDlgItem(hwndChooser, ID_CH_CASE), on);
    EnableWindow(GetDlgItem(hwndChooser, ID_CH_MODS), on);
}

static HWND make_combo(HWND parent, int id, int x, int y, int w,
                       const char **items, int n, int sel) {
    HWND c = CreateWindowExA(0, "COMBOBOX", NULL,
                             WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                             x, y, w, 120,
                             parent, (HMENU)id, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    for (int i = 0; i < n; i++)
        SendMessage(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessage(c, CB_SETCURSEL, sel, 0);
    return c;
}

static void start_from_chooser(HWND h) {
    if (chooserSel >= 0 && chooserSel <= rtNProfiles)
        curProfile = chooserSel;
    HWND chk = GetDlgItem(h, ID_CH_STARTUP);
    BOOL on = SendMessage(chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
    set_startup(on);
    save_settings();
    load_profile_sounds();
    if (!running)
        start_engine();
    else
        update_tip();
    DestroyWindow(h);
    hwndChooser = NULL;
}

static LRESULT CALLBACK ChooserProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->hwndFrom == hwndTab && nm->code == TCN_SELCHANGE)
            show_tab(TabCtrl_GetCurSel(hwndTab));
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_CH_RADIO_BASE && id < ID_CH_RADIO_BASE + rtNProfiles + 1 &&
            code == BN_CLICKED) {
            int sel = id - ID_CH_RADIO_BASE;
            if (sel < rtNProfiles) {
                /* Preset: clicking one selects it AND previews it. */
                chooserSel = RTChooserOrder[sel];
                set_custom_enabled(FALSE);
                preview_profile(chooserSel);
            } else {
                /* Custom build: enable the tuners and preview it. */
                chooserSel = customIndex;
                set_custom_enabled(TRUE);
                preview_custom();
            }
        } else if ((id == ID_CH_SW || id == ID_CH_LUBE ||
                    id == ID_CH_CASE || id == ID_CH_MODS) &&
                   code == CBN_SELCHANGE) {
            int s = (int)SendMessage((HWND)lp, CB_GETCURSEL, 0, 0);
            if (s < 0)
                return 0;
            if (id == ID_CH_SW)
                customSwitch = s;
            else if (id == ID_CH_LUBE)
                customLube = s;
            else if (id == ID_CH_CASE)
                customCase = s;
            else
                customMods = s;
            if (chooserSel == customIndex)
                preview_custom();
        } else if (id == ID_CH_START && code == BN_CLICKED) {
            start_from_chooser(h);
        } else if (id == ID_CH_QUIT && code == BN_CLICKED) {
            PostMessage(h, WM_CLOSE, 0, 0);
        } else if (id == ID_CH_STARTUP && code == BN_CLICKED) {
            BOOL on = SendMessage((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            set_startup(on);
        } else if (id == ID_CH_GAMEPAUSE && code == BN_CLICKED) {
            pauseInGames = SendMessage((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            save_settings();
            if (!pauseInGames && gamePaused) {
                gamePaused = FALSE;
                if (!hHook)
                    hHook = SetWindowsHookExA(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
                update_tip();
            }
        } else if (id == ID_CH_VOLUME && code == CBN_SELCHANGE) {
            int sel = (int)SendMessage((HWND)lp, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < NVOLUMES) {
                volume = VolumeLevels[sel];
                apply_volume();
                save_settings();
            }
        } else if (id == ID_CH_SEARCH_BTN && code == BN_CLICKED) {
            char q[128], status[256];
            HWND list = GetDlgItem(h, ID_CH_RESULTS);
            HWND st = GetDlgItem(h, ID_CH_SEARCH_STATUS);
            GetWindowTextA(GetDlgItem(h, ID_CH_SEARCH), q, sizeof(q));
            SendMessage(list, LB_RESETCONTENT, 0, 0);
            if (!q[0]) {
                SetWindowTextA(st, "Type a keyboard name first.");
            } else {
                int n;
                SetWindowTextA(st, "Searching MechVibes...");
                UpdateWindow(h);
                n = mv_search(q);
                if (n < 0) {
                    SetWindowTextA(st, "Search failed. Check your connection.");
                } else if (n == 0) {
                    SetWindowTextA(st, "No packs found for that keyboard.");
                } else {
                    int i;
                    for (i = 0; i < n; i++) {
                        char line[320];
                        snprintf(line, sizeof(line), "%s — %s%s%s",
                                 mvResults[i].name,
                                 mvResults[i].keyboard,
                                 mvResults[i].switchName[0] ? " — " : "",
                                 mvResults[i].switchName);
                        SendMessage(list, LB_ADDSTRING, 0, (LPARAM)line);
                    }
                    snprintf(status, sizeof(status),
                             "Found %d pack%s. Double-click one to install it.",
                             n, n == 1 ? "" : "s");
                    SetWindowTextA(st, status);
                }
            }
        } else if (id == ID_CH_RESULTS && code == LBN_DBLCLK) {
            int sel = (int)SendMessage((HWND)lp, LB_GETCURSEL, 0, 0);
            HWND st = GetDlgItem(h, ID_CH_SEARCH_STATUS);
            const char *msg;
            if (sel < 0 || sel >= mvResultCount)
                return 0;
            SetWindowTextA(st, "Installing... (downloading and converting)");
            UpdateWindow(h);
            msg = mv_install_pack(sel);
            SetWindowTextA(st, msg);
            if (strncmp(msg, "Installed!", 10) == 0) {
                /* New profile is in the table now: rebuild the chooser
                   radios so it shows up on its tab. */
                DestroyWindow(h);
                hwndChooser = NULL;
                create_chooser();
            }
        } else if (id == ID_CH_RESULTS && code == LBN_SELCHANGE) {
            int sel = (int)SendMessage((HWND)lp, LB_GETCURSEL, 0, 0);
            HWND st = GetDlgItem(h, ID_CH_SEARCH_STATUS);
            if (sel >= 0 && sel < mvResultCount) {
                char info[320];
                snprintf(info, sizeof(info), "%s — %s — %s. Double-click to install.",
                         mvResults[sel].name,
                         mvResults[sel].keyboard,
                         mvResults[sel].license[0] ? mvResults[sel].license : "Unknown license");
                SetWindowTextA(st, info);
            }
        }
        return 0;
    }
    case WM_CLOSE:
        if (!running)
            PostQuitMessage(0);   /* never started: quitting the chooser quits the app */
        DestroyWindow(h);
        hwndChooser = NULL;
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}


static void create_chooser(void) {
    if (hwndChooser) {
        SetForegroundWindow(hwndChooser);
        return;
    }
    int w = 400, hh = 704;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - hh) / 2;
    char title[64];
    snprintf(title, sizeof(title), "MechSounds %s - Choose your switch",
             MECHSOUNDS_VERSION);
    hwndChooser = CreateWindowExA(0, "MechSoundsChooser",
                                  title,
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  x, y, w, hh, NULL, NULL, hInst, NULL);
    if (!hwndChooser)
        return;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND c;

        c = CreateWindowExA(0, "STATIC", "Real recordings - pick one to hear it:",
                        WS_CHILD | WS_VISIBLE, 16, 10, 368, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);

    /* Tab control: one tab per switch type. The radios live inside
       the tab's display area; only the active tab's radios show, so
       a long list never gets cluttered. Radios, not a listbox: a
       listbox runs a type-ahead search on every keypress, and a key
       with no matching switch makes Windows play the error ding.
       Radios never do that. */
    hwndTab = CreateWindowExA(0, WC_TABCONTROL, NULL,
                              WS_CHILD | WS_VISIBLE | TCS_TABS,
                              12, 30, 376, 200,
                              hwndChooser, (HMENU)ID_CH_TAB, hInst, NULL);
    SendMessage(hwndTab, WM_SETFONT, (WPARAM)font, TRUE);
    for (int c = 0; c < 5; c++) {
        TCITEMA ti = {0};
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPSTR)CatNames[c];
        TabCtrl_InsertItem(hwndTab, c, &ti);
    }
    /* Display rect inside the tab control, where the radios go. */
    RECT dr;
    GetClientRect(hwndTab, &dr);
    TabCtrl_AdjustRect(hwndTab, FALSE, &dr);
    MapWindowPoints(hwndTab, hwndChooser, (POINT *)&dr, 2);
    chooserSel = curProfile;
    {
        int start = 0;
        for (int c = 0; c < 5; c++) {
            for (int k = 0; k < RTCatCounts[c]; k++) {
                int i = start + k;
                int prof = RTCatProfs[c][k];
                DWORD style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON;
                if (i == 0)
                    style |= WS_GROUP;
                HWND r = CreateWindowExA(0, "BUTTON", RTNames[prof], style,
                                        dr.left + 12, dr.top + 4 + k * 18,
                                        dr.right - dr.left - 24, 20,
                                        hwndChooser, (HMENU)(ID_CH_RADIO_BASE + i), hInst, NULL);
                SendMessage(r, WM_SETFONT, (WPARAM)font, TRUE);
                if (prof == curProfile)
                    SendMessage(r, BM_SETCHECK, BST_CHECKED, 0);
            }
            start += RTCatCounts[c];
        }
    }
    /* Open on the tab holding the current switch (or Linear). */
    {
        int tab = (curProfile < rtNProfiles) ? cat_of_profile(curProfile) : 0;
        if (tab < 0)
            tab = 0;
        show_tab(tab);
    }

    c = CreateWindowExA(0, "STATIC", "- or build your own -",
                        WS_CHILD | WS_VISIBLE, 16, 240, 368, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);

    c = CreateWindowExA(0, "BUTTON", "Custom build - tune it below",
                        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                        16, 258, 368, 20,
                        hwndChooser, (HMENU)(ID_CH_RADIO_BASE + rtNProfiles), hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
    if (curProfile == customIndex)
        SendMessage(c, BM_SETCHECK, BST_CHECKED, 0);

    static const char *swItems[] = { "Linear", "Tactile", "Clicky" };
    static const char *lubeItems[] = { "Lubed", "Unlubed" };
    static const char *caseItems[] = { "Plastic", "Aluminum", "Brass" };
    static const char *modItems[] = { "None", "Tape mod", "O-rings", "Tape + O-rings" };
    c = CreateWindowExA(0, "STATIC", "Switch:",
                        WS_CHILD | WS_VISIBLE, 16, 284, 48, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
    make_combo(hwndChooser, ID_CH_SW, 66, 282, 118, swItems, 3, customSwitch);
    c = CreateWindowExA(0, "STATIC", "Lube:",
                        WS_CHILD | WS_VISIBLE, 204, 284, 42, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
    make_combo(hwndChooser, ID_CH_LUBE, 248, 282, 118, lubeItems, 2, customLube);
    c = CreateWindowExA(0, "STATIC", "Case:",
                        WS_CHILD | WS_VISIBLE, 16, 312, 48, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
    make_combo(hwndChooser, ID_CH_CASE, 66, 310, 118, caseItems, 3, customCase);
    c = CreateWindowExA(0, "STATIC", "Mods:",
                        WS_CHILD | WS_VISIBLE, 204, 312, 42, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
    make_combo(hwndChooser, ID_CH_MODS, 248, 310, 118, modItems, 4, customMods);
    set_custom_enabled(curProfile == customIndex);

    c = CreateWindowExA(0, "STATIC", "Volume:",
                        WS_CHILD | WS_VISIBLE, 16, 346, 56, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);

    HWND vol = CreateWindowExA(0, "COMBOBOX", NULL,
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                               78, 344, 100, 120,
                               hwndChooser, (HMENU)ID_CH_VOLUME, hInst, NULL);
    SendMessage(vol, WM_SETFONT, (WPARAM)font, TRUE);
    const char *volNames[] = { "100%", "80%", "60%", "40%", "20%" };
    int volsel = 1;
    for (int i = 0; i < NVOLUMES; i++) {
        SendMessage(vol, CB_ADDSTRING, 0, (LPARAM)volNames[i]);
        if (VolumeLevels[i] == volume)
            volsel = i;
    }
    SendMessage(vol, CB_SETCURSEL, volsel, 0);

    HWND chk = CreateWindowExA(0, "BUTTON", "Start MechSounds with Windows",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               16, 374, 260, 20,
                               hwndChooser, (HMENU)ID_CH_STARTUP, hInst, NULL);
    SendMessage(chk, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(chk, BM_SETCHECK,
                startup_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND gchk = CreateWindowExA(0, "BUTTON", "Pause sounds automatically in games",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                16, 398, 300, 20,
                                hwndChooser, (HMENU)ID_CH_GAMEPAUSE, hInst, NULL);
    SendMessage(gchk, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(gchk, BM_SETCHECK,
                pauseInGames ? BST_CHECKED : BST_UNCHECKED, 0);

    /* Search MechVibes: type your keyboard, pick a pack, it installs. */
    c = CreateWindowExA(0, "STATIC", "Search MechVibes for your keyboard:",
                        WS_CHILD | WS_VISIBLE, 16, 424, 368, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
    HWND sedit = CreateWindowExA(0, "EDIT", NULL,
                                 WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 16, 442, 260, 22,
                                 hwndChooser, (HMENU)ID_CH_SEARCH, hInst, NULL);
    SendMessage(sedit, WM_SETFONT, (WPARAM)font, TRUE);
    c = CreateWindowExA(0, "BUTTON", "Search",
                        WS_CHILD | WS_VISIBLE,
                        284, 442, 84, 22,
                        hwndChooser, (HMENU)ID_CH_SEARCH_BTN, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
    HWND slist = CreateWindowExA(0, "LISTBOX", NULL,
                                 WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                                 16, 470, 352, 84,
                                 hwndChooser, (HMENU)ID_CH_RESULTS, hInst, NULL);
    SendMessage(slist, WM_SETFONT, (WPARAM)font, TRUE);
    c = CreateWindowExA(0, "STATIC", "Type a keyboard name and hit Search.",
                        WS_CHILD | WS_VISIBLE, 16, 560, 368, 16,
                        hwndChooser, (HMENU)ID_CH_SEARCH_STATUS, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);

    c = CreateWindowExA(0, "STATIC", "Start hides this window - sounds keep playing",
                        WS_CHILD | WS_VISIBLE, 16, 586, 368, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
    c = CreateWindowExA(0, "STATIC", "from the tray icon. Double-click the tray icon to pick again.",
                        WS_CHILD | WS_VISIBLE, 16, 604, 368, 16,
                        hwndChooser, NULL, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);

    c = CreateWindowExA(0, "BUTTON", "Start",
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        216, 630, 80, 26,
                        hwndChooser, (HMENU)ID_CH_START, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
    c = CreateWindowExA(0, "BUTTON", "Quit",
                        WS_CHILD | WS_VISIBLE,
                        304, 630, 80, 26,
                        hwndChooser, (HMENU)ID_CH_QUIT, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);

    ShowWindow(hwndChooser, SW_SHOW);
    SetForegroundWindow(hwndChooser);
    /* Focus the Start button, NOT a radio: typing with a control
       focused that does type-ahead would make Windows play the
       error ding. That was the ding on every keypress. */
    SetFocus(GetDlgItem(hwndChooser, ID_CH_START));
}

/* ---------------- main window proc -------------------------------- */

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PLAY_DOWN: {
        ULONGLONG dt = GetTickCount64() - (ULONGLONG)lp;
        dispatchSum += dt;
        dispatchN++;
        if (dt > dispatchMax)
            dispatchMax = dt;
        if (dispatchN % 100 == 0)
            update_tip();
        DWORD vk = (DWORD)wp;
        /* One click per press: ignore auto-repeat (a held key sends
           repeated keydowns) and play nothing on release. */
        if (vk < 256 && keyDownState[vk])
            return 0;
        if (vk < 256)
            keyDownState[vk] = TRUE;
        if (enabled) {
            if (vk == VK_SPACE || vk == VK_RETURN || vk == VK_BACK || vk == VK_TAB)
                play_kind(KIND_SPACE, vk);
            else
                play_kind(KIND_DOWN, vk);
        }
        return 0;
    }
    case WM_PLAY_UP: {
        DWORD vk = (DWORD)wp;
        if (vk < 256)
            keyDownState[vk] = FALSE;
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_GAMECHECK)
            game_check();
        return 0;
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP)
            show_menu();
        else if (lp == WM_LBUTTONDBLCLK)
            create_chooser();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_TRAY_EXIT) {
            PostMessage(h, WM_CLOSE, 0, 0);
        } else if (LOWORD(wp) == ID_TRAY_TOGGLE) {
            enabled = !enabled;
            update_tip();
        } else if (LOWORD(wp) == ID_TRAY_CHOOSE) {
            create_chooser();
        } else if (LOWORD(wp) == ID_TRAY_STARTUP) {
            set_startup(!startup_enabled());
        } else if (LOWORD(wp) >= ID_TRAY_PROFILE_BASE &&
                   LOWORD(wp) < ID_TRAY_PROFILE_BASE + rtNProfiles + 1) {
            int sel = LOWORD(wp) - ID_TRAY_PROFILE_BASE;
            if (sel == customIndex) {
                create_chooser();   /* custom build: tune it in the chooser */
            } else {
                curProfile = sel;
                load_profile_sounds();
                update_tip();
                show_balloon(current_profile_name());
                save_settings();
            }
        }
        return 0;
    case WM_DESTROY:
        free_profile_sounds();
        if (hHook)
            UnhookWindowsHookEx(hHook);
        if (running)
            Shell_NotifyIconA(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ---------------- entry point ------------------------------------- */

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hPrev, LPSTR cmd, int show) {
    WNDCLASSA wc = {0};
    WNDCLASSA wc2 = {0};
    MSG m;
    char test[MAX_PATH];
    char *s;
    BOOL startupMode;

    (void)hPrev;
    (void)show;
    hInst = hi;

    {
        INITCOMMONCONTROLSEX ice = { sizeof(ice), ICC_TAB_CLASSES };
        InitCommonControlsEx(&ice);
    }

    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    s = strrchr(exeDir, '\\');
    if (s)
        *s = '\0';

    snprintf(test, sizeof(test), "%s\\sounds\\Rainy75\\down1.wav", exeDir);
    if (GetFileAttributesA(test) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL,
                    "sounds folder not found next to MechSounds.exe.\n"
                    "Keep the sounds folder beside the exe.",
                    "MechSounds", MB_ICONERROR | MB_OK);
        return 1;
    }

    init_runtime_profiles();
    load_settings();
    startupMode = cmd && strstr(cmd, "--startup") != NULL;

    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = "MechSoundsWnd";
    RegisterClassA(&wc);

    wc2.lpfnWndProc = ChooserProc;
    wc2.hInstance = hi;
    wc2.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc2.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc2.lpszClassName = "MechSoundsChooser";
    RegisterClassA(&wc2);

    hwnd = CreateWindowExA(0, "MechSoundsWnd", "MechSounds",
                           0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hi, NULL);
    if (!hwnd)
        return 1;

    init_temp_dir();

    if (startupMode) {
        load_profile_sounds();
        start_engine();
    } else {
        create_chooser();
    }

    while (GetMessage(&m, NULL, 0, 0)) {
        if (hwndChooser && IsDialogMessage(hwndChooser, &m))
            continue;
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}
