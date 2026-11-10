/* KCD DLSS - ASI plugin for Kingdom Come: Deliverance (KCD1). Prototype, milestone 1 (M0):
 *   load, catch the game's D3D11 device, initialise NVIDIA NGX on it, log the DLSS capability check,
 *   compute the optimal render sizes, create and release one DLSS feature. Nothing else is changed.
 *
 * Rules (same as the grass fix): no game file is changed; no C runtime, only kernel32/advapi32;
 * everything unexpected = log it and do nothing; one log file next to the plugin.
 *
 * NGX access: the NGX core (nvngx.dll / _nvngx.dll, or OptiScaler's drop-in) is loaded at run time
 * and its plain C exports are used. Parameters are set through the NVSDK_NGX_Parameter C++ object,
 * whose vtable has MSVC's layout: adjacent overloads in REVERSE declaration order (verified in
 * Crysis 3 Remastered's statically linked NGX code, see WORKING_NOTES.md "Build route").
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "sig.h"

#ifdef KCDDLSS_DEBUG
#define DBG 1
#define BUILD_TAG " [debug build]"
#else
#define DBG 0   /* release build: diagnostics (probe, NGX chatter, post-effect tracing) compiled out, console gets the bare minimum */
#define BUILD_TAG ""
#endif
#define PLUGIN_TITLE "KCD DLSS 0.8.0 (M3: DLSS modes, render presets, live mip bias; prerequisites forced + watchdog; r_DLSSMode/r_DLSSPreset menu cvars) + 30 FPS Grass Fix + Disable Forced Conversation Sharpening" BUILD_TAG
#define NGX_VERSION_API 0x15u
#define NGX_ENGINE_TYPE_CUSTOM 0
#define NGX_FEATURE_SUPERSAMPLING 1
#define NGX_PROJECT_ID "7d3c0e9a-6b4f-4c1e-9f2a-3c5d7e9f1b2d"   /* stable UUID (hex only, NGX validates it) */

/* ---- tiny runtime ---- */
void *memcpy(void *d, const void *s, size_t n) { BYTE *a = d; const BYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { BYTE *a = d; while (n--) *a++ = (BYTE)c; return d; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static __attribute__((unused)) int strieq_w(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR x = *a, y = *b;
        if (x >= L'A' && x <= L'Z') x += 32;
        if (y >= L'A' && y <= L'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int strieq_a(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static DWORD wlen(const WCHAR *s) { DWORD n = 0; while (s[n]) n++; return n; }
static void wcat(WCHAR *d, DWORD cap, const WCHAR *s) { DWORD n = wlen(d); while (*s && n < cap - 1) d[n++] = *s++; d[n] = 0; }

/* ---- log ---- */
static WCHAR g_log_path[MAX_PATH + 8];
static WCHAR g_log_prev[MAX_PATH + 16];   /* 0.7.1: the previous run's log survives as KCDDLSS.prev.log (crash forensics) */
static WCHAR g_dir[MAX_PATH + 8];          /* folder of the plugin (Bin\Win64), no trailing slash */
/* KCDDLSS.ini next to the plugin: Key = Value per line. Defaults below. */
/* mode: -1 Auto (the game's resolution decides), else the NGX PerfQuality value (0 Performance, 1 Balanced, 2 Quality, 3 UltraPerformance, 4 UltraQuality [0.7.4: 0.83 render scale, factor table only], 5 Native/DLAA);
 * preset: 0 Transformer, 1 CNN, 2 Default, 3 one letter (preset_val) for every mode; mipoff: added to log2(render/native) (NVIDIA/AMD -1.0, Intel 0) */
static struct { int mipbias, objectmv, probe, quality, jitter, velpatch, dlss, mode, preset, preset_val, blackms, prereq, gifull, menurows; float sharpness, mipoff; char exec[512]; WCHAR ngxdll[64]; } g_cfg = { 1, 1, 1, -1, 1, 1, 1, -1, 0, 0, 0, 1, 1, 1, 0.0f, -1.0f, "", L"" };   /* BlackMs 0 since 0.7.1: the menu fades by itself (user 2026-11-08) */
static int g_log_open;
static char g_line[600];
static DWORD g_len;
static int g_to_console;   /* set by LOGC: this line is also printed in the game console (ILog, "[KCDDLSS] " prefix) */
static CRITICAL_SECTION g_log_cs;

static void log_s(const char *s) { while (*s && g_len < sizeof g_line - 2) g_line[g_len++] = *s++; }
static void log_x(ULONG_PTR v)
{
    char t[16]; int i = 0;
    do { t[i++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    log_s("0x");
    while (i && g_len < sizeof g_line - 2) g_line[g_len++] = t[--i];
}
static void log_u(DWORD v)
{
    char t[10]; int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i && g_len < sizeof g_line - 2) g_line[g_len++] = t[--i];
}
static void log_i(int v) { if (v < 0) { log_s("-"); v = -v; } log_u((DWORD)v); }
static void log_w(const WCHAR *s) { while (*s && g_len < sizeof g_line - 2) g_line[g_len++] = (*s < 128) ? (char)*s : '?', s++; }
static void game_log(const char *line, int len);
static void log_end(void)
{
    HANDLE h; DWORD w;
    g_line[g_len++] = '\r'; g_line[g_len++] = '\n';
    if (g_log_path[0]) {
        if (!g_log_open) {
            int k = 0, dot = -1;
            while (g_log_path[k] && k < MAX_PATH) { g_log_prev[k] = g_log_path[k]; if (g_log_path[k] == L'.') dot = k; k++; }
            if (dot < 0) dot = k;
            { const WCHAR *ext = L".prev.log"; int j = 0; while (ext[j]) g_log_prev[dot + j] = ext[j], j++; g_log_prev[dot + j] = 0; }
            MoveFileExW(g_log_path, g_log_prev, MOVEFILE_REPLACE_EXISTING);
        }
        h = CreateFileW(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        g_log_open ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            g_log_open = 1;
            SetFilePointer(h, 0, NULL, FILE_END);
            WriteFile(h, g_line, g_len, &w, NULL);
            CloseHandle(h);
        }
    }
    if (g_to_console) game_log(g_line, g_len - 2);
    g_to_console = 0;
    g_len = 0;
}
static void log_init(HINSTANCE self)
{
    DWORD n = GetModuleFileNameW(self, g_log_path, MAX_PATH), i, slash = 0;
    if (!n || n >= MAX_PATH) { g_log_path[0] = 0; return; }
    for (i = 0; i < n; i++) if (g_log_path[i] == L'\\' || g_log_path[i] == L'/') slash = i;
    memcpy(g_dir, g_log_path, slash * sizeof(WCHAR)); g_dir[slash] = 0;
    for (i = n; i > slash; i--) if (g_log_path[i - 1] == L'.') { n = i - 1; break; }
    g_log_path[n] = L'.'; g_log_path[n + 1] = L'l'; g_log_path[n + 2] = L'o'; g_log_path[n + 3] = L'g'; g_log_path[n + 4] = 0;
}
/* LOG: KCDDLSS.log only. LOGC: also the game console + kcd.log (0.7.3: the bare minimum: title, mode/preset changes, CreateFeature,
 * failures). DLOG: debug build only (diagnostics). */
#define LOG(...) do { EnterCriticalSection(&g_log_cs); __VA_ARGS__; log_end(); LeaveCriticalSection(&g_log_cs); } while (0)
#define LOGC(...) do { EnterCriticalSection(&g_log_cs); g_to_console = 1; __VA_ARGS__; log_end(); LeaveCriticalSection(&g_log_cs); } while (0)
#if DBG
#define DLOG(...) LOG(__VA_ARGS__)
#else
#define DLOG(...) do { } while (0)
#endif

static int ci_eq(const char *a, const char *b)   /* case-insensitive key compare, b is the lowercase key */
{
    for (; *a && *b; a++, b++) { char c = *a; if (c >= 'A' && c <= 'Z') c += 32; if (c != *b) return 0; }
    return *a == 0 && *b == 0;
}
static float parse_num(const char *v)
{
    float r = 0, f = 0.1f; int neg = 0;
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '-') { neg = 1; v++; }
    while (*v >= '0' && *v <= '9') { r = r * 10 + (*v - '0'); v++; }
    if (*v == '.') { v++; while (*v >= '0' && *v <= '9') { r += (*v - '0') * f; f *= 0.1f; v++; } }
    return neg ? -r : r;
}
static char *cfg_word(char *v) { char *e; while (*v == ' ' || *v == '\t') v++; e = v; while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != ';' && *e != '\n') e++; *e = 0; return v; }
/* DLSS mode names -> NGX PerfQuality value; -1 Auto (keep the game's resolution), -2 unknown */
static int mode_parse(const char *s)
{
    if (ci_eq(s, "auto")) return -1;
    if (ci_eq(s, "performance") || ci_eq(s, "perf") || ci_eq(s, "0")) return 0;
    if (ci_eq(s, "balanced") || ci_eq(s, "1")) return 1;
    if (ci_eq(s, "quality") || ci_eq(s, "2")) return 2;
    if (ci_eq(s, "ultraperformance") || ci_eq(s, "ultra") || ci_eq(s, "3")) return 3;
    if (ci_eq(s, "ultraquality") || ci_eq(s, "uq") || ci_eq(s, "4")) return 4;
    if (ci_eq(s, "native") || ci_eq(s, "dlaa") || ci_eq(s, "5")) return 5;
    return -2;
}
static const char *MODE_NAMES[6] = { "Performance", "Balanced", "Quality", "UltraPerformance", "UltraQuality", "Native" };
/* render preset (nvsdk_ngx_defs.h, DLSS 310.x): Transformer = K for Native/Quality/Balanced, M for Performance, L for UltraPerformance (the
 * header's stated defaults); CNN = E, F for UltraPerformance/Native (the DLSS 3.x defaults, "deprecated" but shipped); Default = 0; or one letter E..O */
static int preset_parse(const char *s, int *val)
{
    if (ci_eq(s, "transformer") || ci_eq(s, "tf")) return 0;
    if (ci_eq(s, "cnn")) return 1;
    if (ci_eq(s, "default")) return 2;
    if (s[0] && !s[1]) { char c = s[0]; if (c >= 'a' && c <= 'z') c = (char)(c - 32); if (c >= 'E' && c <= 'O') { *val = c - 'A' + 1; return 3; } }
    return -1;
}
static void cfg_load(void)
{
    WCHAR path[MAX_PATH + 40]; HANDLE h; char buf[4096]; DWORD n = 0; char *line, *end, *eq, *v, *k; int i;
    path[0] = 0; wcat(path, MAX_PATH + 40, g_dir); wcat(path, MAX_PATH + 40, L"\\KCDDLSS.ini");
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LOG(log_s("config: no KCDDLSS.ini, defaults (MipBias 1, ObjectMV 1, Probe 1, Quality auto, Sharpness 0)")); return; }
    if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL)) n = 0;
    CloseHandle(h);
    buf[n] = 0;
    for (line = buf; *line; line = end) {
        end = line; while (*end && *end != '\n') end++;
        if (*end) *end++ = 0;
        k = line; while (*k == ' ' || *k == '\t') k++;
        if (*k == ';' || *k == '#' || *k == '[' || *k == 0 || *k == '\r') continue;
        eq = k; while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        v = eq + 1; i = (int)(eq - k); while (i > 0 && (k[i - 1] == ' ' || k[i - 1] == '\t')) i--; k[i] = 0;
        if (ci_eq(k, "mipbias")) g_cfg.mipbias = (int)parse_num(v);
        else if (ci_eq(k, "objectmv")) g_cfg.objectmv = (int)parse_num(v);
        else if (ci_eq(k, "probe")) g_cfg.probe = (int)parse_num(v);
        else if (ci_eq(k, "quality")) g_cfg.quality = (int)parse_num(v);
        else if (ci_eq(k, "sharpness")) g_cfg.sharpness = parse_num(v);
        else if (ci_eq(k, "jitter")) g_cfg.jitter = (int)parse_num(v);
        else if (ci_eq(k, "velocitypatch")) g_cfg.velpatch = (int)parse_num(v);
        else if (ci_eq(k, "menurows")) g_cfg.menurows = (int)parse_num(v);
        else if (ci_eq(k, "dlss")) g_cfg.dlss = (int)parse_num(v);
        else if (ci_eq(k, "mode")) { int m = mode_parse(cfg_word(v)); if (m != -2) g_cfg.mode = m; }
        else if (ci_eq(k, "preset")) { int pv = 0, pk = preset_parse(cfg_word(v), &pv); if (pk >= 0) { g_cfg.preset = pk; g_cfg.preset_val = pv; } }
        else if (ci_eq(k, "mipbiasoffset")) g_cfg.mipoff = parse_num(v);
        else if (ci_eq(k, "blackms")) g_cfg.blackms = (int)parse_num(v);
        else if (ci_eq(k, "prereq")) g_cfg.prereq = (int)parse_num(v);
        else if (ci_eq(k, "gifullres")) g_cfg.gifull = (int)parse_num(v);
        else if (ci_eq(k, "ngxdll")) { const char *w = cfg_word(v); int j = 0; while (w[j] && j < 63) { g_cfg.ngxdll[j] = (WCHAR)(unsigned char)w[j]; j++; } g_cfg.ngxdll[j] = 0; }
        else if (ci_eq(k, "exec")) { int j = 0; while (*v == ' ' || *v == '\t') v++; while (v[j] && v[j] != '\r' && j < (int)sizeof g_cfg.exec - 1) { g_cfg.exec[j] = v[j]; j++; } g_cfg.exec[j] = 0; }
    }
    if (!DBG) g_cfg.probe = 0;   /* the probe exists in the debug build only */
    LOG(log_s("config: KCDDLSS.ini MipBias "); log_i(g_cfg.mipbias); log_s(", ObjectMV "); log_i(g_cfg.objectmv); log_s(", Probe "); log_i(g_cfg.probe);
        log_s(", Quality "); log_i(g_cfg.quality); log_s(" (-1 auto), Sharpness "); log_i((int)(g_cfg.sharpness * 100)); log_s("%, Jitter "); log_i(g_cfg.jitter); log_s(", VelocityPatch "); log_i(g_cfg.velpatch); log_s(", DLSS "); log_i(g_cfg.dlss); log_s(", MenuRows "); log_i(g_cfg.menurows);
        log_s(", Mode "); log_i(g_cfg.mode); log_s(" (-1 auto), Preset "); log_i(g_cfg.preset); log_s("/"); log_i(g_cfg.preset_val); log_s(", MipBiasOffset "); log_i((int)(g_cfg.mipoff * 1000)); log_s(" milli, Prereq "); log_i(g_cfg.prereq); log_s(", GIFullRes "); log_i(g_cfg.gifull); log_s(", NgxDll \""); log_w(g_cfg.ngxdll); log_s("\", Exec \""); log_s(g_cfg.exec); log_s("\""));
}

/* ---- NGX ABI (plain C view of nvsdk_ngx.h / nvsdk_ngx_params.h) ---- */
typedef struct NGX_Parameter NGX_Parameter;      /* opaque C++ object; first qword = vtable */
typedef struct NGX_Handle NGX_Handle;
typedef unsigned int NGX_Result;
#define NGX_OK 1u

typedef struct { const WCHAR *const *Path; unsigned int Length; } NGX_PathListInfo;
typedef void (*NGX_AppLogCallback)(const char *message, int level, int component);
typedef struct { NGX_AppLogCallback LoggingCallback; int MinimumLoggingLevel; unsigned char DisableOtherLoggingSinks; } NGX_LoggingInfo;
typedef struct { NGX_PathListInfo PathListInfo; void *InternalData; NGX_LoggingInfo LoggingInfo; } NGX_FeatureCommonInfo;

typedef NGX_Result (*PFN_D3D11_Init_ProjectID)(const char *id, int engine, const char *ver, const WCHAR *appData,
                                               ID3D11Device *dev, unsigned int sdkVer, const NGX_FeatureCommonInfo *info);
typedef NGX_Result (*PFN_D3D11_Init_Ext)(unsigned long long appId, const WCHAR *appData, ID3D11Device *dev,
                                         unsigned int sdkVer, const NGX_FeatureCommonInfo *info);
typedef NGX_Result (*PFN_D3D11_Shutdown1)(ID3D11Device *dev);
typedef NGX_Result (*PFN_D3D11_Params)(NGX_Parameter **out);
typedef NGX_Result (*PFN_D3D11_DestroyParameters)(NGX_Parameter *p);
typedef NGX_Result (*PFN_D3D11_CreateFeature)(ID3D11DeviceContext *ctx, int feature, NGX_Parameter *p, NGX_Handle **out);
typedef NGX_Result (*PFN_D3D11_ReleaseFeature)(NGX_Handle *h);
typedef NGX_Result (*PFN_D3D11_EvaluateFeature)(ID3D11DeviceContext *ctx, const NGX_Handle *h, const NGX_Parameter *p, void *cb);
typedef NGX_Result (*PFN_OptimalSettingsCallback)(NGX_Parameter *p);

/* vtable of NVSDK_NGX_Parameter, MSVC layout (overload groups reversed) */
enum {
    VT_SET_VOIDP = 0, VT_SET_D3D12 = 1, VT_SET_D3D11 = 2, VT_SET_INT = 3, VT_SET_UINT = 4, VT_SET_DOUBLE = 5, VT_SET_FLOAT = 6, VT_SET_ULL = 7,
    VT_GET_VOIDP = 8, VT_GET_D3D12 = 9, VT_GET_D3D11 = 10, VT_GET_INT = 11, VT_GET_UINT = 12, VT_GET_DOUBLE = 13, VT_GET_FLOAT = 14, VT_GET_ULL = 15,
    VT_RESET = 16
};
typedef void (*VT_SetU)(NGX_Parameter *, const char *, unsigned int);
typedef void (*VT_SetI)(NGX_Parameter *, const char *, int);
typedef void (*VT_SetF)(NGX_Parameter *, const char *, float);
typedef void (*VT_SetP)(NGX_Parameter *, const char *, void *);
typedef NGX_Result (*VT_GetU)(NGX_Parameter *, const char *, unsigned int *);
typedef NGX_Result (*VT_GetI)(NGX_Parameter *, const char *, int *);
typedef NGX_Result (*VT_GetF)(NGX_Parameter *, const char *, float *);
typedef NGX_Result (*VT_GetP)(NGX_Parameter *, const char *, void **);
#define VT(p, i) ((void **)*(void **)(p))[i]
static void p_set_u(NGX_Parameter *p, const char *n, unsigned int v) { ((VT_SetU)VT(p, VT_SET_UINT))(p, n, v); }
static void p_set_i(NGX_Parameter *p, const char *n, int v) { ((VT_SetI)VT(p, VT_SET_INT))(p, n, v); }
static void p_set_f(NGX_Parameter *p, const char *n, float v) { ((VT_SetF)VT(p, VT_SET_FLOAT))(p, n, v); }
static void p_set_p(NGX_Parameter *p, const char *n, void *v) { ((VT_SetP)VT(p, VT_SET_VOIDP))(p, n, v); }
static NGX_Result p_get_u(NGX_Parameter *p, const char *n, unsigned int *v) { return ((VT_GetU)VT(p, VT_GET_UINT))(p, n, v); }
static NGX_Result p_get_i(NGX_Parameter *p, const char *n, int *v) { return ((VT_GetI)VT(p, VT_GET_INT))(p, n, v); }
static NGX_Result p_get_f(NGX_Parameter *p, const char *n, float *v) { return ((VT_GetF)VT(p, VT_GET_FLOAT))(p, n, v); }
static NGX_Result p_get_p(NGX_Parameter *p, const char *n, void **v) { return ((VT_GetP)VT(p, VT_GET_VOIDP))(p, n, v); }

static int g_core_from_game_dir;   /* NGX core came from the game folder (OptiScaler drop-in or a copied core) */
static int g_no_upscaler;   /* 0.7.4: set at device creation when no NGX core / no NVIDIA + no drop-in: modes keep the native size */
static struct {
    HMODULE core;
    PFN_D3D11_Init_ProjectID InitProjectID;
    PFN_D3D11_Init_Ext InitExt;
    PFN_D3D11_Shutdown1 Shutdown1;
    PFN_D3D11_Params GetCapabilityParameters, AllocateParameters;
    PFN_D3D11_DestroyParameters DestroyParameters;
    PFN_D3D11_CreateFeature CreateFeature;
    PFN_D3D11_ReleaseFeature ReleaseFeature;
    PFN_D3D11_EvaluateFeature EvaluateFeature;
    ID3D11Device *dev;
    int inited;
    int available;
    NGX_Parameter *caps;   /* capability parameters (never destroyed): carry DLSSOptimalSettingsCallback */
} g_ngx;

static void ngx_log_cb(const char *message, int level, int component)
{
    (void)component;
    DLOG(log_s("NGX["); log_i(level); log_s("]: "); log_s(message ? message : ""));   /* NGX chatter: debug build only */
}

static HMODULE ngx_load_core(void)
{
    static const WCHAR *names[] = { L"nvngx.dll", L"_nvngx.dll" };
    static const struct { const WCHAR *key, *val; } regs[] = {
        { L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore", L"NGXPath" },
        { L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath" },
        { L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath" },
    };
    WCHAR path[MAX_PATH + 40]; HMODULE h; DWORD i, k, sz;
    /* 0. KCDDLSS.ini NgxDll = <file in Bin\Win64>: an alternative NGX provider, e.g. OptiScaler.dll (FSR 3.1 / FSR 4 / XeSS) */
    if (g_cfg.ngxdll[0]) {
        path[0] = 0; wcat(path, MAX_PATH + 40, g_dir); wcat(path, MAX_PATH + 40, L"\\"); wcat(path, MAX_PATH + 40, g_cfg.ngxdll);
        h = LoadLibraryW(path);
        if (h) { g_core_from_game_dir = 1; LOG(log_s("NGX provider from KCDDLSS.ini NgxDll: "); log_w(path)); return h; }
        LOG(log_s("NgxDll "); log_w(path); log_s(" did not load (error "); log_u(GetLastError()); log_s("): falling back to the normal NGX core search"));
    }
    /* 1. next to the game (an OptiScaler drop-in or a copied core wins) */
    for (i = 0; i < 2; i++) {
        path[0] = 0; wcat(path, MAX_PATH + 40, g_dir); wcat(path, MAX_PATH + 40, L"\\"); wcat(path, MAX_PATH + 40, names[i]);
        h = LoadLibraryW(path);
        if (h) { g_core_from_game_dir = 1; LOG(log_s("NGX core from the game folder: "); log_w(path)); return h; }
    }
    /* 2. the driver's NGXCore folder from the registry */
    for (k = 0; k < 3; k++) {
        sz = sizeof path - 40 * sizeof(WCHAR); path[0] = 0;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, regs[k].key, regs[k].val, RRF_RT_REG_SZ, NULL, path, &sz) != ERROR_SUCCESS || !path[0]) continue;
        for (i = 0; i < 2; i++) {
            WCHAR full[MAX_PATH + 40]; full[0] = 0;
            wcat(full, MAX_PATH + 40, path);
            if (full[wlen(full) - 1] != L'\\') wcat(full, MAX_PATH + 40, L"\\");
            wcat(full, MAX_PATH + 40, i ? names[0] : names[1]);           /* _nvngx.dll first (drivers 471.11+) */
            h = LoadLibraryW(full);
            if (h) { LOG(log_s("NGX core from the driver: "); log_w(full)); return h; }
        }
    }
    /* 3. plain search path */
    h = LoadLibraryW(L"_nvngx.dll"); if (!h) h = LoadLibraryW(L"nvngx.dll");
    if (h) LOG(log_s("NGX core from the search path"));
    return h;
}

static int ngx_resolve(void)
{
    HMODULE c = g_ngx.core;
    g_ngx.InitProjectID = (PFN_D3D11_Init_ProjectID)(void *)GetProcAddress(c, "NVSDK_NGX_D3D11_Init_ProjectID");
    g_ngx.InitExt = (PFN_D3D11_Init_Ext)(void *)GetProcAddress(c, "NVSDK_NGX_D3D11_Init_Ext");
    g_ngx.Shutdown1 = (PFN_D3D11_Shutdown1)(void *)GetProcAddress(c, "NVSDK_NGX_D3D11_Shutdown1");
    g_ngx.GetCapabilityParameters = (PFN_D3D11_Params)(void *)GetProcAddress(c, "NVSDK_NGX_D3D11_GetCapabilityParameters");
    g_ngx.AllocateParameters = (PFN_D3D11_Params)(void *)GetProcAddress(c, "NVSDK_NGX_D3D11_AllocateParameters");
    g_ngx.DestroyParameters = (PFN_D3D11_DestroyParameters)(void *)GetProcAddress(c, "NVSDK_NGX_D3D11_DestroyParameters");
    g_ngx.CreateFeature = (PFN_D3D11_CreateFeature)(void *)GetProcAddress(c, "NVSDK_NGX_D3D11_CreateFeature");
    g_ngx.ReleaseFeature = (PFN_D3D11_ReleaseFeature)(void *)GetProcAddress(c, "NVSDK_NGX_D3D11_ReleaseFeature");
    g_ngx.EvaluateFeature = (PFN_D3D11_EvaluateFeature)(void *)GetProcAddress(c, "NVSDK_NGX_D3D11_EvaluateFeature");
    LOG(log_s("NGX exports: Init_ProjectID "); log_s(g_ngx.InitProjectID ? "yes" : "no");
        log_s(", Init_Ext "); log_s(g_ngx.InitExt ? "yes" : "no");
        log_s(", Shutdown1 "); log_s(g_ngx.Shutdown1 ? "yes" : "no");
        log_s(", GetCapabilityParameters "); log_s(g_ngx.GetCapabilityParameters ? "yes" : "no");
        log_s(", AllocateParameters "); log_s(g_ngx.AllocateParameters ? "yes" : "no");
        log_s(", Create/Release/Evaluate "); log_s(g_ngx.CreateFeature && g_ngx.ReleaseFeature && g_ngx.EvaluateFeature ? "yes" : "no"));
    return (g_ngx.InitProjectID || g_ngx.InitExt) && g_ngx.GetCapabilityParameters && g_ngx.AllocateParameters &&
           g_ngx.DestroyParameters && g_ngx.CreateFeature && g_ngx.ReleaseFeature && g_ngx.EvaluateFeature;
}

static void log_get_i(NGX_Parameter *p, const char *name)
{
    int v = -12345; NGX_Result r = p_get_i(p, name, &v);
    LOG(log_s("  "); log_s(name); log_s(" = "); if (r == NGX_OK) log_i(v); else { log_s("<not set, result "); log_x(r); log_s(">"); });
}

/* optimal render size for one output size and quality; exercises Set(uint) and Get(uint)/Get(void*) slots */
static void ngx_log_optimal(unsigned int ow, unsigned int oh, int quality, const char *qname, unsigned int *rw_out, unsigned int *rh_out)
{
    NGX_Parameter *p = NULL; PFN_OptimalSettingsCallback cb = NULL; unsigned int w = 0, h = 0, minw = 0, minh = 0, maxw = 0, maxh = 0;
    float sharp = 0; NGX_Result r; int own = 0;
    if (rw_out) *rw_out = 0;
    if (rh_out) *rh_out = 0;
    p = g_ngx.caps;   /* the optimal-settings callback is only present on the capability parameters (nvsdk_ngx_helpers.h) */
    if (!p) { if (g_ngx.AllocateParameters(&p) != NGX_OK || !p) { LOG(log_s("AllocateParameters failed")); return; } own = 1; }
    p_set_u(p, "Width", ow); p_set_u(p, "Height", oh);
    p_set_u(p, "OutWidth", ow); p_set_u(p, "OutHeight", oh);
    p_set_i(p, "PerfQualityValue", quality);
    p_set_i(p, "RTXValue", 0);
    r = p_get_p(p, "DLSSOptimalSettingsCallback", (void **)&cb);
    if (r != NGX_OK || !cb) { LOG(log_s("  no DLSSOptimalSettingsCallback (result "); log_x(r); log_s(")")); if (own) g_ngx.DestroyParameters(p); return; }
    r = cb(p);
    p_get_u(p, "OutWidth", &w); p_get_u(p, "OutHeight", &h);
    p_get_u(p, "DLSS.Get.Dynamic.Min.Render.Width", &minw); p_get_u(p, "DLSS.Get.Dynamic.Min.Render.Height", &minh);
    p_get_u(p, "DLSS.Get.Dynamic.Max.Render.Width", &maxw); p_get_u(p, "DLSS.Get.Dynamic.Max.Render.Height", &maxh);
    p_get_f(p, "Sharpness", &sharp);
    LOG(log_s("  optimal "); log_u(ow); log_s("x"); log_u(oh); log_s(" "); log_s(qname); log_s(": render "); log_u(w); log_s("x"); log_u(h);
        log_s(" (min "); log_u(minw); log_s("x"); log_u(minh); log_s(", max "); log_u(maxw); log_s("x"); log_u(maxh); log_s("), callback result "); log_x(r));
    if (r == NGX_OK && w >= 64 && h >= 64 && w <= ow && h <= oh) { if (rw_out) *rw_out = w; if (rh_out) *rh_out = h; }
    if (own) g_ngx.DestroyParameters(p);
}

static void ngx_init(ID3D11Device *dev)
{
    static const WCHAR *paths[1]; NGX_FeatureCommonInfo info; NGX_Parameter *caps = NULL; NGX_Result r;
    int avail = 0;
    if (g_ngx.inited) {
        LOG(log_s("NGX already initialised on device "); log_x((ULONG_PTR)g_ngx.dev); log_s(": shutting it down first"));
        if (g_ngx.Shutdown1) g_ngx.Shutdown1(g_ngx.dev);
        g_ngx.inited = 0; g_ngx.available = 0;
    }
    if (!g_ngx.core || !g_ngx.CreateFeature) { LOGC(log_s("no usable NGX core (nvngx.dll / _nvngx.dll): DLSS unavailable, nothing changed")); return; }
    paths[0] = g_dir;
    memset(&info, 0, sizeof info);
    info.PathListInfo.Path = paths; info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = ngx_log_cb; info.LoggingInfo.MinimumLoggingLevel = 1; info.LoggingInfo.DisableOtherLoggingSinks = 0;
    if (g_ngx.InitProjectID) {
        r = g_ngx.InitProjectID(NGX_PROJECT_ID, NGX_ENGINE_TYPE_CUSTOM, "0.2.0", g_dir, dev, NGX_VERSION_API, &info);
        LOG(log_s("NVSDK_NGX_D3D11_Init_ProjectID: result "); log_x(r));
    } else {
        r = g_ngx.InitExt(0x24480451ull, g_dir, dev, NGX_VERSION_API, &info);
        LOG(log_s("NVSDK_NGX_D3D11_Init_Ext: result "); log_x(r));
    }
    if (r != NGX_OK) { LOGC(log_s("NGX init failed: DLSS unavailable, nothing changed")); return; }
    g_ngx.inited = 1; g_ngx.dev = dev;

    r = g_ngx.GetCapabilityParameters(&caps);
    if (r != NGX_OK || !caps) { LOG(log_s("GetCapabilityParameters: result "); log_x(r)); return; }
    LOG(log_s("capabilities:"));
    log_get_i(caps, "SuperSampling.Available");
    log_get_i(caps, "SuperSampling.NeedsUpdatedDriver");
    log_get_i(caps, "SuperSampling.MinDriverVersionMajor");
    log_get_i(caps, "SuperSampling.MinDriverVersionMinor");
    log_get_i(caps, "SuperSampling.FeatureInitResult");
    log_get_i(caps, "Snippet.OptLevel");
    log_get_i(caps, "Snippet.IsDevBranch");
    { unsigned long long raw = 0; NGX_Result rr = ((NGX_Result (*)(NGX_Parameter *, const char *, unsigned long long *))VT(caps, VT_GET_ULL))(caps, "SuperSampling.Available", &raw);
      LOG(log_s("  slot check: Get(ull) SuperSampling.Available = "); log_x((ULONG_PTR)raw); log_s(", result "); log_x(rr)); }
    if (p_get_i(caps, "SuperSampling.Available", &avail) == NGX_OK && avail) g_ngx.available = 1;
    g_ngx.caps = caps;
    if (!g_ngx.available) { LOGC(log_s("DLSS not available on this GPU/driver: nothing changed")); return; }

}

/* ---- 30 FPS Grass Fix (merged from KCDGrassFix 1.0.0, same code, same signature) ----
 * The game simulates grass in fixed 35 ms steps and holds the pose in between. The patch adds a small
 * piece of code after the simulation that shifts each grass point along its speed by the time still
 * missing to the next step, so the drawn pose moves every frame. Found by bytes, not by address. */
static int same(const BYTE *a, const BYTE *b, SIZE_T n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static int readable(const BYTE *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi; const BYTE *end = p + n;
    while (p < end) {
        if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        p = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}
static BYTE *alloc_near(const BYTE *target)
{
    SYSTEM_INFO si; MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR gran, lo, hi, t = (ULONG_PTR)target, p; BYTE *r; int guard;
    GetSystemInfo(&si);
    gran = si.dwAllocationGranularity;
    lo = t > 0x7ff00000u ? t - 0x7ff00000u : 0;
    hi = t + 0x7ff00000u;
    if (lo < (ULONG_PTR)si.lpMinimumApplicationAddress) lo = (ULONG_PTR)si.lpMinimumApplicationAddress;
    if (hi > (ULONG_PTR)si.lpMaximumApplicationAddress || hi < t) hi = (ULONG_PTR)si.lpMaximumApplicationAddress;
    for (p = t & ~(gran - 1), guard = 0; p > lo && guard < 200000; guard++) {
        if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE) {
            r = VirtualAlloc((void *)p, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (r) return r;
            p -= gran;
        } else {
            ULONG_PTR ab = (ULONG_PTR)mbi.AllocationBase;
            p = (ab && ab < p ? ab : p) - gran;
            p &= ~(gran - 1);
        }
    }
    for (p = (t + gran) & ~(gran - 1), guard = 0; p < hi && guard < 200000; guard++) {
        if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE) {
            r = VirtualAlloc((void *)p, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (r) return r;
            p += gran;
        } else {
            p = ((ULONG_PTR)mbi.BaseAddress + mbi.RegionSize + gran - 1) & ~(gran - 1);
        }
    }
    return NULL;
}
static void put32(BYTE *p, INT64 v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }
static BYTE *g_grass_base;

static void grass_patch(BYTE *base)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base; IMAGE_NT_HEADERS64 *nt; IMAGE_SECTION_HEADER *sec;
    BYTE *hit = NULL, *hook, *code, jmp[HOOK_LEN]; DWORD i, old, tmp; int hits = 0; INT64 rel;
    if (!base || base == g_grass_base) return;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        BYTE *s = base + sec->VirtualAddress, *p, *end; SIZE_T n = sec->Misc.VirtualSize;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (n < SIG_PRE_LEN + HOOK_LEN + SIG_POST_LEN || !readable(s, n)) continue;
        end = s + n - (SIG_PRE_LEN + HOOK_LEN + SIG_POST_LEN);
        for (p = s; p <= end; p++) {
            SIZE_T k;
            if (p[0] != SIG_PRE[0] || p[1] != SIG_PRE[1] || p[2] != SIG_PRE[2] || p[3] != SIG_PRE[3]) continue;
            for (k = 4; k < SIG_PRE_LEN; k++) if (SIG_PRE_MASK[k] && p[k] != SIG_PRE[k]) break;
            if (k == SIG_PRE_LEN) { if (!hits) hit = p; hits++; }
        }
    }
    if (hits != 1) { LOG(log_s("grass: code found "); log_u((DWORD)hits); log_s(" times (need exactly 1): unsupported game version, grass unchanged")); return; }
    hook = hit + SIG_PRE_LEN;
    if (!same(hook + HOOK_LEN, SIG_POST, SIG_POST_LEN)) { LOGC(log_s("grass: code after the grass loop differs: unsupported game version, grass unchanged")); return; }
    if (!same(hook, HOOK_ORIG, HOOK_LEN)) {
        if (hook[0] == 0xE9 && hook[5] == 0x0F && hook[6] == 0x1F && hook[7] == 0x44 && hook[8] == 0 && hook[9] == 0)
            LOG(log_s("grass: already fixed (patched WHGame.dll or the standalone KCDGrassFix.asi): nothing to do"));
        else
            LOGC(log_s("grass: loop already changed by something else: grass unchanged"));
        return;
    }
    code = alloc_near(hook);
    if (!code) { LOGC(log_s("grass: no free memory near WHGame.dll: grass unchanged")); return; }
    memcpy(code, CAVE, CAVE_LEN);
    rel = (INT64)(ULONG_PTR)(hook + HOOK_LEN) - (INT64)(ULONG_PTR)(code + CAVE_TAIL_OFF + 5);
    if (rel > 0x7fffffffLL || rel < -0x7fffffffLL - 1) { VirtualFree(code, 0, MEM_RELEASE); LOGC(log_s("grass: new code out of jump range: grass unchanged")); return; }
    put32(code + CAVE_TAIL_OFF + 1, rel);
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &old)) { VirtualFree(code, 0, MEM_RELEASE); LOGC(log_s("grass: cannot make the new code executable: grass unchanged")); return; }
    FlushInstructionCache(GetCurrentProcess(), code, CAVE_LEN);
    rel = (INT64)(ULONG_PTR)code - (INT64)(ULONG_PTR)(hook + 5);
    jmp[0] = 0xE9; put32(jmp + 1, rel);
    jmp[5] = 0x0F; jmp[6] = 0x1F; jmp[7] = 0x44; jmp[8] = 0x00; jmp[9] = 0x00;
    if (!VirtualProtect(hook, HOOK_LEN, PAGE_EXECUTE_READWRITE, &old)) { VirtualFree(code, 0, MEM_RELEASE); LOGC(log_s("grass: cannot write to WHGame.dll's code: grass unchanged")); return; }
    memcpy(hook, jmp, HOOK_LEN);
    VirtualProtect(hook, HOOK_LEN, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), hook, HOOK_LEN);
    g_grass_base = base;
    LOG(log_s("grass fix applied: hook at WHGame.dll+"); log_x((ULONG_PTR)(hook - base)); log_s(", new code at "); log_x((ULONG_PTR)code));
}

/* ---- D3D11CreateDevice interception via WHGame.dll's GetProcAddress import ---- */
typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static PFN_D3D11CreateDevice g_real_create;
static FARPROC (WINAPI *g_real_gpa)(HMODULE, LPCSTR);
static BYTE *g_hooked_base;

static int g_devices;
static const GUID IID_IDXGIDevice_local = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const GUID IID_Tex2D_local = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const GUID IID_SRV_local = { 0xb0e06fe0, 0x8192, 0x4e1a, { 0xb1, 0xca, 0x36, 0xd7, 0x41, 0x47, 0x10, 0xb2 } };

/* adapter vendor of a device: 0x10de NVIDIA, 0x1002 AMD, 0x8086 Intel, 0x1414 Microsoft (Basic Render Driver) */
static UINT device_vendor(ID3D11Device *dev, WCHAR *desc, UINT desc_cap)
{
    IDXGIDevice *dxd = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC d; UINT vendor = 0;
    desc[0] = 0;
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice_local, (void **)&dxd)) || !dxd) return 0;
    if (SUCCEEDED(IDXGIDevice_GetAdapter(dxd, &ad)) && ad) {
        if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d))) {
            UINT i; vendor = d.VendorId;
            for (i = 0; i < desc_cap - 1 && d.Description[i]; i++) desc[i] = d.Description[i];
            desc[i] = 0;
        }
        IDXGIAdapter_Release(ad);
    }
    IDXGIDevice_Release(dxd);
    return vendor;
}

static void hook_sampler_state(ID3D11Device *dev);
static void hook_ctx(ID3D11DeviceContext *ctx); static void aa_pin_install(const char *src);
static HRESULT WINAPI my_D3D11CreateDevice(IDXGIAdapter *a, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL *fl, UINT nfl,
                                           UINT sdk, ID3D11Device **dev, D3D_FEATURE_LEVEL *outfl, ID3D11DeviceContext **ctx)
{
    HRESULT hr = g_real_create(a, dt, sw, flags, fl, nfl, sdk, dev, outfl, ctx);
    LOG(log_s("D3D11CreateDevice: hr "); log_x((ULONG_PTR)(DWORD)hr); log_s(", driver type "); log_u(dt); log_s(", flags "); log_x(flags);
        log_s(", device "); log_x(dev ? (ULONG_PTR)*dev : 0); log_s(", feature level "); log_x(outfl ? (ULONG_PTR)*outfl : 0));
    if (SUCCEEDED(hr) && dev && *dev && dt != D3D_DRIVER_TYPE_NULL && dt != D3D_DRIVER_TYPE_REFERENCE) {
        WCHAR desc[64]; UINT vendor;
        if (!g_ngx.core) { g_ngx.core = ngx_load_core(); if (g_ngx.core && !ngx_resolve()) { LOGC(log_s("NGX core lacks required D3D11 exports: DLSS unavailable")); } }
        vendor = device_vendor(*dev, desc, 64);
        g_devices++;
        LOG(log_s("  adapter vendor "); log_x(vendor); log_s(" \""); log_w(desc); log_s("\" (device #"); log_u((DWORD)g_devices); log_s(")"));
        aa_pin_install("device");
        /* 0.7.4: decide now whether any upscaler can run, so the modes keep the native render size otherwise (Steam Deck without
         * the OptiScaler Proton, 2026-11-08: the plugin set 640x400 and the game showed its own blocky stretch) */
        if (!g_no_upscaler && vendor && vendor != 0x1414 && (!g_ngx.core || (vendor != 0x10de && !g_core_from_game_dir))) {
            g_no_upscaler = 1;
            LOGC(log_s("dlss: "); log_s(!g_ngx.core ? "no NGX core found" : "not an NVIDIA adapter and no OptiScaler / drop-in core");
                 log_s(": upscaling off, the render size stays native (AMD / Intel: see OPTISCALER.txt)"));
        }
        /* NGX is initialised lazily on the device the renderer really uses (first upscaled frame, see dlss_frame) */
        if (vendor != 0x1414) {
            ID3D11DeviceContext *ic = NULL;   /* 0.6.5: hook the immediate context now, not at the first stretch: the frame tick in
                                                 OMSetRenderTargets applies the startup settings even when the game boots at native size */
            hook_sampler_state(*dev);
            if (ctx && *ctx) { ic = *ctx; ID3D11DeviceContext_AddRef(ic); } else ID3D11Device_GetImmediateContext(*dev, &ic);
            if (ic) { hook_ctx(ic); ID3D11DeviceContext_Release(ic); }
        }
    }
    return hr;
}

static FARPROC WINAPI my_GetProcAddress(HMODULE h, LPCSTR name)
{
    FARPROC real = g_real_gpa(h, name);
    if (real && ((ULONG_PTR)name >> 16) != 0 && streq(name, "D3D11CreateDevice")) {
        g_real_create = (PFN_D3D11CreateDevice)(void *)real;
        LOG(log_s("game asked for D3D11CreateDevice ("); log_x((ULONG_PTR)real); log_s("): handing it the hook"));
        return (FARPROC)(void *)my_D3D11CreateDevice;
    }
    return real;
}

/* ================= Milestone 1: DLSS replaces the game's UpscaleImage stretch =================
 * KCD renders the scene and ALL post-processing at r_Width x r_Height and, at the very end, stretches the final
 * LDR image ($SceneSpecular) to the native-size back buffer with a smoothstep filter (renderer vtable +0xab8 ->
 * 0x18029b51c -> UpscaleImage 0x1820901e4; see FINDINGS_KCD.md Q2). The UI is drawn after that at native size.
 * We replace that vtable entry. Our hook lets the game's stretch run (fallback picture, and it binds everything we
 * need), then reads back from the immediate context what the stretch used: the render target (back buffer, native
 * size) and the pixel-shader texture at slot 0 ($SceneSpecular, render size). The hardware depth view is the global
 * the game binds at t16. DLSS (LDR mode, depth inverted, auto exposure, low-res motion vectors) upscales into our own
 * native-size texture, which is copied over the back buffer. Milestone 1a uses an all-zero motion-vector texture
 * (camera motion is added in 1b), so only a still camera is judged.
 * Jitter: the game adds the float2 at renderer+0xf20 (NDC units, see Q3) to the projection matrix every frame but
 * recomputes it only in AA mode 3 and ZEROES it otherwise (0x1803e243f). We NOP that zeroing and write our own
 * Halton(2,3) sequence there at the end of each frame, for the next frame; the same offset (in pixels, C3R's sign
 * convention: -x, +y) goes to DLSS as Jitter.Offset.
 * Requirements (user.cfg): r_Width/r_Height = render size, r_NativeWidth/r_NativeHeight = output size (or
 * fullscreen native res), r_AntialiasingMode 0. Anything unexpected: log once, leave the game's stretch result. */
/* --- per-build WHGame.dll offsets ---
 * Every fixed WHGame.dll address the plugin uses, one row per game build; the RVA_* names below read from the selected
 * row. select_build() picks the row once in hook_whgame() by reading (never writing) two identity checks: the renderer
 * vtable slot +0xab8 holds that build's stretch function, and CSystem slots 111/112 hold its LoadXmlFromBuffer /
 * LoadXmlFromFile (the same comparisons dlss_install and menu_xml_install make before hooking). Either check is enough,
 * so a vtable already hooked by another mod does not hide the build; a 1/2 match is logged as partial. Every patch
 * still checks its own bytes or slot before writing. With no matching row the build is unknown: only the
 * pattern-searched grass fix and sharpening removal run, dlss_install / menu_xml_install are skipped and g_wh_base
 * stays NULL, which turns off everything that reads WHGame.dll globals.
 * Epic 1.9.8b row: from the Epic WHGame.dll (PE timestamp 0x69ccdca1, SizeOfImage 0x39e5000), each value matched to
 * its use in code (patch-site bytes, RTTI vtables, gEnv stores, cvar registrations, call sites), not computed by a
 * shift. The data shifts against Steam differ by group: renderer, depth SRV, camera matrices, bind records -0x5480;
 * $VelocityObjects -0x5410; eye index -0x54a4; AA copy -0x5484; r_PostProcessGameFx/r_CustomVisions -0x54ec;
 * gEnv -0x5040. Do not derive a new row from one shift.
 * Not per build (shared by all rows, so a new build must re-check them too): the struct offsets R_*, R_DEVINFO,
 * R_FULLSCREEN, PFX_MGR_OFF, CT_* and the vtable slot numbers (renderer, console, CryPak, post effects). They are
 * unchanged in Epic 1.9.8b (the renderer's depth SRV member sits 0x29130 from the object in both builds). Addresses
 * quoted in comments elsewhere in this file are Steam 1.9.8 ones. */
typedef struct {
    const char *name;
    DWORD renderer, vt_upscale, fn_upscale, depth_srv, velocity_tex, stereo_eye, jitter_zero, aa_mode, plog, console, post_view, post_proj, bindrec, gamefx, customvis, velfmt_l, velfmt_r, csys_vt, fn_loadxml_file, fn_loadxml_buf, pcrypak, fn_onactivate;
    BYTE ctx_l[23];      /* velocity_fmt_patch: the 20 bytes before and 3 at the left/mono creation site */
    BYTE zero_ctx[16];   /* dlss_install: jne rel32 + mov al,1 + the 8-byte jitter zeroing */
} WH_BUILD;
static const WH_BUILD g_wh_builds[] = {
    { .name = "Steam 1.9.8",
      .renderer = 0x34c6980u, .vt_upscale = 0x23e9b68u, .fn_upscale = 0x29b51cu, .depth_srv = 0x34efab0u,
      .velocity_tex = 0x378f6d8u, .stereo_eye = 0x2ff12b4u, .jitter_zero = 0x3e243fu, .aa_mode = 0x358fcc4u,
      .plog = 0x29d1798u, .console = 0x29d1768u, .post_view = 0x34b95b8u, .post_proj = 0x34b95f8u,
      .bindrec = 0x33ff908u, .gamefx = 0x3786b84u, .customvis = 0x3786b88u, .velfmt_l = 0x2020671u,
      .velfmt_r = 0x20206bcu, .csys_vt = 0x268a6a8u, .fn_loadxml_file = 0xea3d98u, .fn_loadxml_buf = 0xea3d70u,
      .pcrypak = 0x29d1710u, .fn_onactivate = 0x666a24u,
      .ctx_l = { 0x44,0x8b,0x05,0xa4,0x52,0x4b,0x01, 0x48,0x8d,0x05,0x6d,0xf0,0x76,0x01, 0x8b,0x15,0x93,0x52,0x4b,0x01, 0x41,0xb1,0x0b },
      .zero_ctx = { 0x0f,0x85,0x8c,0xcb,0x86,0x00,0xb0,0x01, 0x49,0x83,0xa7,0x20,0x0f,0x00,0x00,0x00 } },
    { .name = "Epic 1.9.8b",
      .renderer = 0x34c1500u, .vt_upscale = 0x23e17a8u, .fn_upscale = 0x29b74cu, .depth_srv = 0x34ea630u,
      .velocity_tex = 0x378a2c8u, .stereo_eye = 0x2febe10u, .jitter_zero = 0x3e262fu, .aa_mode = 0x358a840u,
      .plog = 0x29cc758u, .console = 0x29cc728u, .post_view = 0x34b4138u, .post_proj = 0x34b4178u,
      .bindrec = 0x33fa488u, .gamefx = 0x3781698u, .customvis = 0x378169cu, .velfmt_l = 0x201cd35u,
      .velfmt_r = 0x201cd80u, .csys_vt = 0x26818d8u, .fn_loadxml_file = 0xea06bcu, .fn_loadxml_buf = 0xea0694u,
      .pcrypak = 0x29cc6d0u, .fn_onactivate = 0x666c14u,
      .ctx_l = { 0x44,0x8b,0x05,0x60,0x37,0x4b,0x01, 0x48,0x8d,0x05,0x99,0xd5,0x76,0x01, 0x8b,0x15,0x4f,0x37,0x4b,0x01, 0x41,0xb1,0x0b },
      .zero_ctx = { 0x0f,0x85,0xf8,0x92,0x86,0x00,0xb0,0x01, 0x49,0x83,0xa7,0x20,0x0f,0x00,0x00,0x00 } },
};
static const WH_BUILD *g_wh_build = &g_wh_builds[0];
#define RVA_RENDERER    (g_wh_build->renderer)   /* CD3D9Renderer instance (static object) */
#define RVA_VT_UPSCALE  (g_wh_build->vt_upscale)   /* renderer vtable entry +0xab8 */
#define RVA_FN_UPSCALE  (g_wh_build->fn_upscale)    /* expected value of that entry */
#define RVA_DEPTH_SRV   (g_wh_build->depth_srv)   /* ID3D11ShaderResourceView* of the hardware depth buffer (bound at t16) */
#define RVA_VELOCITY_TEX (g_wh_build->velocity_tex)  /* CTexture* $VelocityObjects[2] (slot 1 = right eye); FINDINGS_KCD_CTEXTURE.md */
#define RVA_STEREO_EYE  (g_wh_build->stereo_eye)   /* int: current eye index (0 mono) */
#define ETF_R16G16F 0x0e            /* CryEngine eTF for R16G16_FLOAT (patched in for $VelocityObjects) */
#define CT_DEVTEX 0x30u              /* CTexture+0x30 CDeviceTexture* -> +0 ID3D11Resource* */
#define CT_WIDTH 0x48u               /* u16 width, +0x4a u16 height, +0x4e u8 eTF (0x0b = R8G8S) */
#define CT_SRV 0xc0u                 /* default ID3D11ShaderResourceView* */
#define RVA_JITTER_ZERO (g_wh_build->jitter_zero)    /* and qword ptr [r15+0xf20], 0 (8 bytes) in the jitter generator */
#define RVA_AA_MODE     (g_wh_build->aa_mode)   /* effective r_AntialiasingMode copy */
#define RVA_PLOG        (g_wh_build->plog)   /* ILog* (gEnv->pLog); slot 2 = LogV(type, fmt, va_list), type 3 = eAlways -> game console + kcd.log (FINDINGS_KCD_LOG.md) */
typedef void (*PFN_LogV)(void *log, int type, const char *fmt, char *va);
static BYTE *g_wh_base;   /* tentative definition; the real one with its comment follows below */
/* 0.7.2: every plugin log line also goes into the game's drop-down console and kcd.log, prefixed [KCDDLSS] (user: "same way the FPS fix
 * does it"). Lines written before WHGame.dll is known stay in KCDDLSS.log only. "%s" + a hand-made va_list: no formatting of our text. */
static void game_log(const char *line, int len)
{
    static char buf[1040]; void *log, **vt; unsigned long long args[1]; int i = 0, j; const char *pre = "[KCDDLSS] ";
    if (!g_wh_base || len <= 0 || !readable(g_wh_base + RVA_PLOG, 8)) return;
    log = *(void **)(g_wh_base + RVA_PLOG);
    if (!log || !readable((BYTE *)log, 8)) return;
    vt = *(void ***)log;
    if (!vt || !readable((BYTE *)vt, 8 * 3) || !vt[2]) return;
    while (*pre) buf[i++] = *pre++;
    for (j = 0; j < len && i < (int)sizeof buf - 1; j++) buf[i++] = line[j];
    buf[i] = 0;
    args[0] = (unsigned long long)(ULONG_PTR)buf;
    ((PFN_LogV)vt[2])(log, 3, "%s", (char *)args);
}
#define RVA_CONSOLE     (g_wh_build->console)   /* IConsole* (gEnv->pConsole copy); GetCVar = vtable slot 23, ICVar::GetIVal = slot 2 */
#define R_DOWNSCALE_X 0x6764
#define R_DOWNSCALE_Y 0x6768
#define R_JITTER_X 0xf20
#define R_JITTER_Y 0xf24
#define R_RENDER_W 0xea90
#define R_RENDER_H 0xea94
#define R_NATIVE_W 0xeaa4
#define R_NATIVE_H 0xeaa8
#define R_SUPERSAMPLE 0xeab4
#define DLSS_FLAGS ((1 << 1) | (1 << 3) | (1 << 6))   /* MVLowRes | DepthInverted | AutoExposure; IsHDR off */

typedef void (*PFN_Stretch)(BYTE *renderer, BYTE draw);
static unsigned int g_css_biased, g_css_total;   /* sampler states created / given a mip bias */
static PFN_Stretch g_orig_stretch;
static BYTE *g_wh_base;

static struct {
    ID3D11Device *dev;
    NGX_Handle *feature; NGX_Parameter *params;
    ID3D11Texture2D *out, *mv;
    UINT rw, rh, nw, nh; DXGI_FORMAT out_fmt;
    int quality, phases, phase, reset, hold, disabled;
    unsigned int frames, errs;
    float jx, jy;             /* pixel jitter in effect for the frame being rendered now */
} g_dl;

/* the saved part of the immediate-context state around EvaluateFeature (the game caches its D3D state and would
 * otherwise believe stale bindings) */
#define NSRV 32
#define NCB 14
#define NSS 16
typedef struct { ID3D11ShaderResourceView *srv[NSRV]; ID3D11Buffer *cb[NCB]; ID3D11SamplerState *ss[NSS]; } StageState;
typedef struct {
    ID3D11RenderTargetView *rtv[8]; ID3D11DepthStencilView *dsv;
    UINT nvp; D3D11_VIEWPORT vp[16]; UINT nsc; D3D11_RECT sc[16];
    StageState vs_st, hs_st, ds_st, gs_st, ps_st, cs_st;
    ID3D11UnorderedAccessView *cs_uav[8];
    ID3D11ComputeShader *cs; ID3D11PixelShader *ps; ID3D11VertexShader *vs; ID3D11GeometryShader *gs; ID3D11HullShader *hs; ID3D11DomainShader *ds;
    ID3D11Buffer *vb[16], *ib; UINT vb_stride[16], vb_off[16], ib_off; DXGI_FORMAT ib_fmt;
    ID3D11BlendState *bs; FLOAT bf[4]; UINT smask; ID3D11DepthStencilState *dss; UINT sref; ID3D11RasterizerState *rs;
    ID3D11InputLayout *il; D3D11_PRIMITIVE_TOPOLOGY topo; ID3D11Predicate *pred; BOOL pred_val;
} CtxState;
#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* complete save/restore of everything DLSS or our MV pass could touch: the game caches its bindings and would keep
 * believing stale ones (one-frame colour glitches, CHANGES row 29-31) */
#define STAGE_GET(C, S, st) do { ID3D11DeviceContext_##S##GetShaderResources(C, 0, NSRV, (st)->srv); ID3D11DeviceContext_##S##GetConstantBuffers(C, 0, NCB, (st)->cb); ID3D11DeviceContext_##S##GetSamplers(C, 0, NSS, (st)->ss); } while (0)
#define STAGE_SET(C, S, st) do { ID3D11DeviceContext_##S##SetShaderResources(C, 0, NSRV, (st)->srv); ID3D11DeviceContext_##S##SetConstantBuffers(C, 0, NCB, (st)->cb); ID3D11DeviceContext_##S##SetSamplers(C, 0, NSS, (st)->ss); } while (0)
static void stage_release(StageState *st)
{
    int i;
    for (i = 0; i < NSRV; i++) REL(st->srv[i]);
    for (i = 0; i < NCB; i++) REL(st->cb[i]);
    for (i = 0; i < NSS; i++) REL(st->ss[i]);
}
static void ctx_save(ID3D11DeviceContext *c, CtxState *s)
{
    memset(s, 0, sizeof *s);
    ID3D11DeviceContext_OMGetRenderTargets(c, 8, s->rtv, &s->dsv);
    s->nvp = 16; ID3D11DeviceContext_RSGetViewports(c, &s->nvp, s->vp);
    s->nsc = 16; ID3D11DeviceContext_RSGetScissorRects(c, &s->nsc, s->sc);
    STAGE_GET(c, VS, &s->vs_st); STAGE_GET(c, HS, &s->hs_st); STAGE_GET(c, DS, &s->ds_st);
    STAGE_GET(c, GS, &s->gs_st); STAGE_GET(c, PS, &s->ps_st); STAGE_GET(c, CS, &s->cs_st);
    ID3D11DeviceContext_CSGetUnorderedAccessViews(c, 0, 8, s->cs_uav);
    ID3D11DeviceContext_CSGetShader(c, &s->cs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &s->ps, NULL, NULL);
    ID3D11DeviceContext_VSGetShader(c, &s->vs, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &s->gs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(c, &s->hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(c, &s->ds, NULL, NULL);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 16, s->vb, s->vb_stride, s->vb_off);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &s->ib, &s->ib_fmt, &s->ib_off);
    ID3D11DeviceContext_OMGetBlendState(c, &s->bs, s->bf, &s->smask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &s->dss, &s->sref);
    ID3D11DeviceContext_RSGetState(c, &s->rs);
    ID3D11DeviceContext_IAGetInputLayout(c, &s->il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &s->topo);
    ID3D11DeviceContext_GetPredication(c, &s->pred, &s->pred_val);
}
static void ctx_restore(ID3D11DeviceContext *c, CtxState *s)
{
    int i;
    /* unbind everything first so that no resource is bound as input and output at the same time while rebinding */
    { ID3D11UnorderedAccessView *nul[8] = { 0 }; ID3D11RenderTargetView *nrt[8] = { 0 };
      ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 8, nul, NULL); ID3D11DeviceContext_OMSetRenderTargets(c, 8, nrt, NULL); }
    ID3D11DeviceContext_CSSetShader(c, s->cs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, s->ps, NULL, 0);
    ID3D11DeviceContext_VSSetShader(c, s->vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, s->gs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, s->hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, s->ds, NULL, 0);
    STAGE_SET(c, VS, &s->vs_st); STAGE_SET(c, HS, &s->hs_st); STAGE_SET(c, DS, &s->ds_st);
    STAGE_SET(c, GS, &s->gs_st); STAGE_SET(c, PS, &s->ps_st); STAGE_SET(c, CS, &s->cs_st);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 8, s->cs_uav, NULL);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 16, s->vb, s->vb_stride, s->vb_off);
    ID3D11DeviceContext_IASetIndexBuffer(c, s->ib, s->ib_fmt, s->ib_off);
    ID3D11DeviceContext_OMSetRenderTargets(c, 8, s->rtv, s->dsv);
    ID3D11DeviceContext_RSSetViewports(c, s->nvp, s->vp);
    ID3D11DeviceContext_RSSetScissorRects(c, s->nsc, s->sc);
    ID3D11DeviceContext_OMSetBlendState(c, s->bs, s->bf, s->smask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, s->dss, s->sref);
    ID3D11DeviceContext_RSSetState(c, s->rs);
    ID3D11DeviceContext_IASetInputLayout(c, s->il);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, s->topo);
    ID3D11DeviceContext_SetPredication(c, s->pred, s->pred_val);
    for (i = 0; i < 8; i++) { REL(s->rtv[i]); REL(s->cs_uav[i]); }
    for (i = 0; i < 16; i++) REL(s->vb[i]);
    REL(s->dsv);
    stage_release(&s->vs_st); stage_release(&s->hs_st); stage_release(&s->ds_st); stage_release(&s->gs_st); stage_release(&s->ps_st); stage_release(&s->cs_st);
    REL(s->cs); REL(s->ps); REL(s->vs); REL(s->gs); REL(s->hs); REL(s->ds); REL(s->ib);
    REL(s->bs); REL(s->dss); REL(s->rs); REL(s->il); REL(s->pred);
}

static float halton(unsigned int i, unsigned int b) { float f = 1.0f, r = 0.0f; while (i) { f /= (float)b; r += f * (float)(i % b); i /= b; } return r; }

static void dl_fail(const char *what)   /* log the first few problems only */
{
    if (g_dl.errs++ < 6) LOG(log_s("dlss: "); log_s(what); log_s(g_dl.errs == 6 ? " (further errors not logged)" : ""));
}

static int tex_desc(ID3D11Resource *res, D3D11_TEXTURE2D_DESC *d)
{
    ID3D11Texture2D *t = NULL;
    memset(d, 0, sizeof *d);
    if (!res || FAILED(ID3D11Resource_QueryInterface(res, &IID_Tex2D_local, (void **)&t)) || !t) return 0;
    ID3D11Texture2D_GetDesc(t, d); ID3D11Texture2D_Release(t);
    return 1;
}

static DXGI_FORMAT uav_format(DXGI_FORMAT f)   /* UAVs cannot be sRGB or typeless; the copy back is bit-exact anyway */
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

static ID3D11ShaderResourceView *depth_view(void)   /* the game's hardware-depth SRV global; validated before use */
{
    ID3D11ShaderResourceView **pp = (ID3D11ShaderResourceView **)(g_wh_base + RVA_DEPTH_SRV), *v, *q = NULL;
    if (!readable((BYTE *)pp, 8)) return NULL;
    v = *pp;
    if (!v || !readable((BYTE *)v, 8) || !readable(*(BYTE **)v, 8 * 4)) return NULL;
    if (FAILED(ID3D11ShaderResourceView_QueryInterface(v, &IID_SRV_local, (void **)&q))) return NULL;
    return q;
}

/* the game's object-velocity buffer ($VelocityObjects, written by moving/skinned objects within r_MotionBlurMaxViewDist;
 * x == 0 means "no object velocity", else prevUV - uv encoded sign*sqrt, *0.5 + 127/255 (Common.cfi EncodeMotionVector)) */
static ID3D11ShaderResourceView *velocity_view(UINT *vw, UINT *vh)
{
    BYTE **slot = (BYTE **)(g_wh_base + RVA_VELOCITY_TEX), *t, *devtex; ID3D11ShaderResourceView *v, *q = NULL; int eye;
    static int logged, warned;
    if (readable(g_wh_base + RVA_STEREO_EYE, 4)) { eye = *(int *)(g_wh_base + RVA_STEREO_EYE); if (eye == 1) slot++; }
    if (!readable((BYTE *)slot, 8)) return NULL;
    t = *slot;
    if (!t || !readable(t, 0xd0)) return NULL;
    if (t[0x4e] != 0x0b && t[0x4e] != ETF_R16G16F) { if (!warned) { warned = 1; LOG(log_s("velocity: $VelocityObjects eTF is "); log_x(t[0x4e]); log_s(", expected 0xb/0xe: object motion vectors off")); } return NULL; }
    *vw = *(USHORT *)(t + CT_WIDTH); *vh = *(USHORT *)(t + CT_WIDTH + 2);
    v = *(ID3D11ShaderResourceView **)(t + CT_SRV);
    if (!v || !readable((BYTE *)v, 8) || !readable(*(BYTE **)v, 8 * 4)) return NULL;
    if (FAILED(ID3D11ShaderResourceView_QueryInterface(v, &IID_SRV_local, (void **)&q)) || !q) return NULL;
    if (!logged) {
        ID3D11Resource *res = NULL; D3D11_TEXTURE2D_DESC d; devtex = *(BYTE **)(t + CT_DEVTEX);
        logged = 1;
        ID3D11ShaderResourceView_GetResource(q, &res);
        LOG(log_s("velocity: $VelocityObjects "); log_u(*vw); log_s("x"); log_u(*vh); log_s(" eTF "); log_x(t[0x4e]); log_s(", srv "); log_x((ULONG_PTR)v); log_s(", resource "); log_x((ULONG_PTR)res);
            log_s(devtex && readable(devtex, 8) && *(ID3D11Resource **)devtex == res ? " (matches CTexture+0x30)" : " (CTexture+0x30 differs!)"));
        if (tex_desc(res, &d)) LOG(log_s("velocity: DXGI format "); log_u(d.Format); log_s(" "); log_u(d.Width); log_s("x"); log_u(d.Height); log_s(" bind "); log_x(d.BindFlags));
        REL(res);
    }
    return q;
}

static void dl_release_feature(void)
{
    if (g_dl.feature) { g_ngx.ReleaseFeature(g_dl.feature); g_dl.feature = NULL; }
    REL(g_dl.out); REL(g_dl.mv);
}

/* render preset hints (one per PerfQuality value, index = NGX PerfQuality: Perf, Balanced, Quality, UltraPerf, UltraQuality, DLAA) */
static const char *PRESET_PARAMS[6] = { "DLSS.Hint.Render.Preset.Performance", "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Quality",
                                        "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality", "DLSS.Hint.Render.Preset.DLAA" };
static unsigned int preset_value(int quality)   /* NVSDK_NGX_DLSS_Hint_Render_Preset value: A = 1 ... E 5, F 6, J 10, K 11, L 12, M 13; 0 = DLL default */
{
    static const unsigned int tf[6] = { 13, 11, 11, 12, 11, 11 };   /* M, K, K, L, K, K */
    static const unsigned int cnn[6] = { 5, 5, 5, 6, 6, 6 };          /* E, E, E, F, F, F */
    if (quality < 0 || quality > 5) quality = 0;
    if (g_cfg.preset == 0) return tf[quality];
    if (g_cfg.preset == 1) return cnn[quality];
    if (g_cfg.preset == 3) return (unsigned int)g_cfg.preset_val;
    return 0;
}
static void preset_apply(NGX_Parameter *p)
{
    int i; unsigned int v = preset_value(g_dl.quality); char letter[2]; static const char *kind[4] = { "Transformer", "CNN", "Default", "letter" };
    for (i = 0; i < 6; i++) p_set_u(p, PRESET_PARAMS[i], preset_value(i));
    letter[0] = v ? (char)('A' + v - 1) : '-'; letter[1] = 0;
    LOG(log_s("dlss: render preset "); log_s(kind[g_cfg.preset & 3]); log_s(": this mode gets preset "); log_s(v ? letter : "DLL default"); log_s(" (value "); log_u(v); log_s(")"));
}
static int ensure_feature(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT rw, UINT rh, UINT nw, UINT nh, DXGI_FORMAT fmt)
{
    D3D11_TEXTURE2D_DESC d; D3D11_SUBRESOURCE_DATA init; void *zero; NGX_Result r; float ratio; HRESULT hr;
    static const char *qname[] = { "Performance", "Balanced", "Quality", "UltraPerformance", "UltraQuality", "DLAA" };
    if (g_dl.feature && g_dl.dev == dev && g_dl.rw == rw && g_dl.rh == rh && g_dl.nw == nw && g_dl.nh == nh && g_dl.out_fmt == fmt) return 1;
    dl_release_feature();
    if (g_ngx.dev != dev || !g_ngx.inited) {
        WCHAR desc[64]; UINT vendor = device_vendor(dev, desc, 64);
        LOG(log_s("dlss: renderer device "); log_x((ULONG_PTR)dev); log_s(" vendor "); log_x(vendor); log_s(" \""); log_w(desc); log_s("\""));
        if (vendor != 0x10de && !g_core_from_game_dir) { LOGC(log_s("dlss: not an NVIDIA adapter and no drop-in core: disabled")); g_dl.disabled = 1; return 0; }
        ngx_init(dev);
        if (!g_ngx.available) { g_dl.disabled = 1; return 0; }
    }
    if (!g_dl.params && (g_ngx.AllocateParameters(&g_dl.params) != NGX_OK || !g_dl.params)) { dl_fail("AllocateParameters failed: disabled"); g_dl.disabled = 1; return 0; }
    ratio = (float)nw / (float)rw;
    g_dl.quality = ratio >= 2.9f ? 3 : ratio >= 1.9f ? 0 : ratio >= 1.6f ? 1 : ratio >= 1.2f ? 2 : 5;
    if (g_cfg.mode >= 0 && g_cfg.mode <= 5) g_dl.quality = g_cfg.mode;
    else if (g_cfg.quality >= 0 && g_cfg.quality <= 5) g_dl.quality = g_cfg.quality;
    if (g_dl.quality != 4) ngx_log_optimal(nw, nh, g_dl.quality, qname[g_dl.quality], NULL, NULL);
    preset_apply(g_dl.params);
    p_set_u(g_dl.params, "Width", rw); p_set_u(g_dl.params, "Height", rh);
    p_set_u(g_dl.params, "OutWidth", nw); p_set_u(g_dl.params, "OutHeight", nh);
    /* 0.7.4: UltraQuality (mode 4, 0.83) is created as PerfQuality Quality: DLSS 310 refuses value 4 (0xbad00010, tested 2026-11-08);
     * the DLL takes any render size, the value only picks the model/preset hint (Quality's K / E) */
    p_set_i(g_dl.params, "PerfQualityValue", g_dl.quality == 4 ? 2 : g_dl.quality);
    p_set_i(g_dl.params, "DLSS.Feature.Create.Flags", DLSS_FLAGS);
    p_set_u(g_dl.params, "DLSS.Enable.Output.Subrects", 0);
    r = g_ngx.CreateFeature(ctx, NGX_FEATURE_SUPERSAMPLING, g_dl.params, &g_dl.feature);
    LOGC(log_s("dlss: CreateFeature "); log_u(rw); log_s("x"); log_u(rh); log_s(" -> "); log_u(nw); log_s("x"); log_u(nh); log_s(" "); log_s(g_dl.quality == 4 ? "UltraQuality (as Quality)" : qname[g_dl.quality]);
        log_s(", flags "); log_x(DLSS_FLAGS); log_s(": result "); log_x(r));
    if (r != NGX_OK || !g_dl.feature) { g_dl.feature = NULL; g_dl.disabled = 1; LOGC(log_s("dlss: feature creation failed: disabled, the game's stretch stays")); return 0; }
    memset(&d, 0, sizeof d);
    d.Width = nw; d.Height = nh; d.MipLevels = 1; d.ArraySize = 1; d.Format = uav_format(fmt); d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_dl.out);
    if (FAILED(hr) || !g_dl.out) { LOG(log_s("dlss: output texture (format "); log_u(d.Format); log_s(") failed: hr "); log_x((ULONG_PTR)(DWORD)hr); log_s(": disabled")); g_dl.out = NULL; dl_release_feature(); g_dl.disabled = 1; return 0; }
    zero = VirtualAlloc(NULL, (SIZE_T)rw * rh * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   /* zero-filled */
    memset(&d, 0, sizeof d);
    d.Width = rw; d.Height = rh; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R16G16_FLOAT; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    init.pSysMem = zero; init.SysMemPitch = rw * 4; init.SysMemSlicePitch = 0;
    hr = ID3D11Device_CreateTexture2D(dev, &d, zero ? &init : NULL, &g_dl.mv);
    if (zero) VirtualFree(zero, 0, MEM_RELEASE);
    if (FAILED(hr) || !g_dl.mv) { LOG(log_s("dlss: motion-vector texture failed: hr "); log_x((ULONG_PTR)(DWORD)hr); log_s(": disabled")); g_dl.mv = NULL; dl_release_feature(); g_dl.disabled = 1; return 0; }
    g_dl.dev = dev; g_dl.rw = rw; g_dl.rh = rh; g_dl.nw = nw; g_dl.nh = nh; g_dl.out_fmt = fmt;
    g_dl.phases = (int)(8.0f * ((float)nw / (float)rw) * ((float)nh / (float)rh));
    if (g_dl.phases < 8) g_dl.phases = 8;
    if (g_dl.phases > 64) g_dl.phases = 64;
    g_dl.phase = 0; g_dl.reset = 1; g_dl.jx = g_dl.jy = 0;
    LOG(log_s("dlss: ready, output format "); log_u(fmt); log_s(" (ours "); log_u(uav_format(fmt)); log_s("), "); log_i(g_dl.phases); log_s(" jitter phases"));
    return 1;
}

/* ---- Milestone 1b: camera motion vectors (own full-screen pass, compiled at run time with D3DCompile) ----
 * For every render-res pixel: take the hardware depth, rebuild the clip position (uv, depth), apply the
 * reprojection matrix (uv/depth of this frame -> uv of the previous frame, built on the CPU from the game's
 * current and previous view-projection matrices), write (prevUV - uv) * renderSize in pixels to the RG16F
 * motion-vector texture (MVLowRes, MV.Scale 1). Where the game has object velocity ($VelocityObjects) a later
 * milestone replaces the camera vector. All D3D state we touch is saved/restored around the pass. */
static const char MV_HLSL[] =
"#pragma pack_matrix(row_major)\n"
"cbuffer C : register(b0) { float4x4 M; float2 size; float2 velSize; float useVel; float3 pad; };\n"
"Texture2D<float> depthTex : register(t0);\n"
"Texture2D<float2> velTex : register(t1);\n"
"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) { V o; float2 t = float2((id << 1) & 2, id & 2); o.uv = t; o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"float2 ps(V i) : SV_Target {\n"
"  float d = depthTex.Load(int3(i.pos.xy, 0));\n"
"  float4 p = mul(float4(i.uv, d, 1), M);\n"
"  float2 mv = p.xy / p.w - i.uv;\n"
"  if (useVel > 0) {\n"
"    float2 e = velTex.Load(int3(i.uv * velSize, 0));\n"
"    if (e.x != 0) { e = (e - 127.0 / 255.0) * 2.0; mv = e * e * (e > 0 ? float2(1, 1) : float2(-1, -1)); }\n"
"  }\n"
"  return mv * size;\n"
"}\n";
typedef HRESULT (WINAPI *PFN_D3DCompile)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, ID3D10Blob **, ID3D10Blob **);
static struct {
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11Buffer *cb; ID3D11RenderTargetView *rtv; ID3D11Texture2D *rtv_of;
    int failed;
} g_mv;
typedef struct { float m[16]; float size[2]; float velSize[2]; float useVel; float pad[3]; } MvConst;

static void mv_release(void) { REL(g_mv.vs); REL(g_mv.ps); REL(g_mv.cb); REL(g_mv.rtv); g_mv.rtv_of = NULL; }

static int mv_compile(ID3D11Device *dev)
{
    HMODULE dc = GetModuleHandleW(L"d3dcompiler_47.dll"); PFN_D3DCompile compile; ID3D10Blob *code = NULL, *err = NULL; HRESULT hr; D3D11_BUFFER_DESC bd;
    if (g_mv.vs && g_mv.ps && g_mv.cb) return 1;
    if (g_mv.failed) return 0;
    if (!dc) dc = LoadLibraryW(L"d3dcompiler_47.dll");
    compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) { LOG(log_s("mv: D3DCompile not available: camera motion vectors off (zeros)")); g_mv.failed = 1; return 0; }
    hr = compile(MV_HLSL, sizeof MV_HLSL - 1, "kcd_dlss_mv", NULL, NULL, "vs", "vs_5_0", (1 << 15) /* OPTIMIZATION_LEVEL3 */, 0, &code, &err);
    if (FAILED(hr) || !code) { LOG(log_s("mv: vertex shader compile failed: "); if (err) log_s((const char *)ID3D10Blob_GetBufferPointer(err))); REL(err); g_mv.failed = 1; return 0; }
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &g_mv.vs);
    REL(code); REL(err);
    if (FAILED(hr)) { LOG(log_s("mv: CreateVertexShader failed: hr "); log_x((ULONG_PTR)(DWORD)hr)); g_mv.failed = 1; return 0; }
    hr = compile(MV_HLSL, sizeof MV_HLSL - 1, "kcd_dlss_mv", NULL, NULL, "ps", "ps_5_0", (1 << 15), 0, &code, &err);
    if (FAILED(hr) || !code) { LOG(log_s("mv: pixel shader compile failed: "); if (err) log_s((const char *)ID3D10Blob_GetBufferPointer(err))); REL(err); mv_release(); g_mv.failed = 1; return 0; }
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &g_mv.ps);
    REL(code); REL(err);
    if (FAILED(hr)) { LOG(log_s("mv: CreatePixelShader failed: hr "); log_x((ULONG_PTR)(DWORD)hr)); mv_release(); g_mv.failed = 1; return 0; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(MvConst); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &g_mv.cb);
    if (FAILED(hr)) { LOG(log_s("mv: constant buffer failed: hr "); log_x((ULONG_PTR)(DWORD)hr)); mv_release(); g_mv.failed = 1; return 0; }
    LOG(log_s("mv: shaders compiled (vs_5_0/ps_5_0), camera motion vectors on"));
    return 1;
}

/* draws the motion vectors into g_dl.mv; M = 4x4 row-major reprojection (uv,depth,1,1 row vector -> prev uv) */
static void mv_pass(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *depth, ID3D11ShaderResourceView *vel, UINT vw, UINT vh, const float *M, UINT rw, UINT rh)
{
    MvConst c; D3D11_VIEWPORT vp; ID3D11ShaderResourceView *srv0[2] = { depth, vel }; ID3D11Buffer *cb0[1]; ID3D11RenderTargetView *rt[1];
    if (!mv_compile(dev)) return;
    if (g_mv.rtv_of != g_dl.mv) {
        REL(g_mv.rtv); g_mv.rtv_of = NULL;
        if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)g_dl.mv, NULL, &g_mv.rtv))) { g_mv.rtv = NULL; dl_fail("mv: render target view failed"); return; }
        g_mv.rtv_of = g_dl.mv;
    }
    memcpy(c.m, M, sizeof c.m); c.size[0] = (float)rw; c.size[1] = (float)rh;
    c.velSize[0] = (float)vw; c.velSize[1] = (float)vh; c.useVel = (vel && vw && vh) ? 1.0f : 0.0f; c.pad[0] = c.pad[1] = c.pad[2] = 0;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_mv.cb, 0, NULL, &c, 0, 0);
    rt[0] = g_mv.rtv; cb0[0] = g_mv.cb;
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)rw; vp.Height = (float)rh; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, rt, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffffu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_mv.vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_HSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_mv.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, cb0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srv0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    srv0[0] = srv0[1] = NULL; ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srv0);
    rt[0] = NULL; ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, rt, NULL);   /* unbind: DLSS reads g_dl.mv next */
}

/* ---- Blue-frame probe (diagnostic, CHANGES row 36): every frame the colour input and the DLSS output are point-sampled
 * on a 32x18 grid into tiny render targets, copied to staging and read back one frame later. A frame whose input is
 * blue-dominant is logged; the first 3 such frames have their full colour input (kept in a 2-deep staging ring)
 * written as KCDDLSS_blue_N.ppm next to the plugin. */
static unsigned int g_bmask[16]; static unsigned int g_bmask_n; static int g_bind_logged;   /* bind_check(), below */

#if DBG
static void fx_log_state(unsigned int back);
#endif
static unsigned int g_fxh_n; static int g_fx_wrapped; static unsigned int g_fx_wrap_calls;   /* post effects module, below */
static void hook_ctx(ID3D11DeviceContext *ctx); static float mip_bias_now(void); static float g_sc_bias; static float g_css_last_bias; static int g_sc_n; static unsigned int g_sc_subst;   /* mip bias module, below */
static int g_dump_req;   /* `dump` console command: save the DLSS input/output next frame */
static int g_black;      /* frames left to black out the 3D view (mode/preset switch: hides the resize + history rebuild, like the game's own settings apply) */
static DWORD g_blink_end;   /* `blink <seconds>` test: 2 s black / 2 s normal until this tick */
static DWORD g_black_end;   /* black-out until this tick (mode/preset switch: covers the resize and the eye-adaptation restart, user 2026-11-08) */
static void black_ms(int ms) { DWORD now = GetTickCount(); if (ms <= 0) { g_black_end = 0; g_black = 0; return; } if ((int)(g_black_end - now) < ms) g_black_end = now + (DWORD)ms; }
#if DBG   /* blue-frame probe: debug build only */
#define PW 32
#define PH 18
static const char PROBE_HLSL[] =
"cbuffer C : register(b0) { float2 size; float2 pad; };\n"
"Texture2D<float4> colTex : register(t0);\n"
"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) { V o; float2 t = float2((id << 1) & 2, id & 2); o.uv = t; o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"float4 ps(V i) : SV_Target { return colTex.Load(int3(int2(i.uv * size), 0)); }\n";
static struct {
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11Buffer *cb;
    ID3D11Texture2D *rt_in, *rt_out, *st_in[2], *st_out[2], *ring[2]; ID3D11RenderTargetView *rtv_in, *rtv_out; ID3D11ShaderResourceView *out_srv;
    ID3D11Texture2D *out_of; UINT rw, rh; DXGI_FORMAT fmt; int failed; unsigned int logs, dumps, frames;
    int pbi, pr, pg, pb, pflag;   /* previous frame's input stats, and whether it was flagged */
    int hist[16][4]; unsigned int hn; int after;   /* last 16 frames: blue%, mean r/g/b; frames still to log after a spike */
} g_pr;
static void probe_release(void)
{
    REL(g_pr.vs); REL(g_pr.ps); REL(g_pr.cb); REL(g_pr.rt_in); REL(g_pr.rt_out); REL(g_pr.rtv_in); REL(g_pr.rtv_out); REL(g_pr.out_srv);
    REL(g_pr.st_in[0]); REL(g_pr.st_in[1]); REL(g_pr.st_out[0]); REL(g_pr.st_out[1]); REL(g_pr.ring[0]); REL(g_pr.ring[1]); g_pr.out_of = NULL; g_pr.rw = g_pr.rh = 0;
}
#endif
static int probe_tex(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, UINT usage, UINT cpu, ID3D11Texture2D **out)
{
    D3D11_TEXTURE2D_DESC d; memset(&d, 0, sizeof d);
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = usage; d.BindFlags = bind; d.CPUAccessFlags = cpu;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, out)) && *out;
}
static void utoa_w(char *dst, unsigned int v) { char t[12]; int i = 0; if (!v) t[i++] = '0'; while (v) { t[i++] = (char)('0' + v % 10); v /= 10; } while (i) *dst++ = t[--i]; *dst = 0; }
#if DBG
static int probe_init(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *cd)
{
    HMODULE dc = GetModuleHandleW(L"d3dcompiler_47.dll"); PFN_D3DCompile compile; ID3D10Blob *code = NULL, *err = NULL; HRESULT hr; D3D11_BUFFER_DESC bd;
    if (g_pr.failed) return 0;
    if (g_pr.rw == cd->Width && g_pr.rh == cd->Height && g_pr.fmt == cd->Format && g_pr.vs) return 1;
    probe_release();
    if (!dc) dc = LoadLibraryW(L"d3dcompiler_47.dll");
    compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) { g_pr.failed = 1; return 0; }
    hr = compile(PROBE_HLSL, sizeof PROBE_HLSL - 1, "kcd_dlss_probe", NULL, NULL, "vs", "vs_5_0", (1 << 15), 0, &code, &err);
    if (FAILED(hr) || !code) { REL(err); g_pr.failed = 1; DLOG(log_s("probe: vs compile failed")); return 0; }
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &g_pr.vs); REL(code); REL(err);
    if (FAILED(hr)) { g_pr.failed = 1; return 0; }
    hr = compile(PROBE_HLSL, sizeof PROBE_HLSL - 1, "kcd_dlss_probe", NULL, NULL, "ps", "ps_5_0", (1 << 15), 0, &code, &err);
    if (FAILED(hr) || !code) { REL(err); g_pr.failed = 1; DLOG(log_s("probe: ps compile failed")); return 0; }
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &g_pr.ps); REL(code); REL(err);
    if (FAILED(hr)) { g_pr.failed = 1; return 0; }
    memset(&bd, 0, sizeof bd); bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &g_pr.cb))) { g_pr.failed = 1; return 0; }
    if (!probe_tex(dev, PW, PH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, &g_pr.rt_in) ||
        !probe_tex(dev, PW, PH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0, &g_pr.rt_out) ||
        !probe_tex(dev, PW, PH, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_pr.st_in[0]) ||
        !probe_tex(dev, PW, PH, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_pr.st_in[1]) ||
        !probe_tex(dev, PW, PH, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_pr.st_out[0]) ||
        !probe_tex(dev, PW, PH, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_pr.st_out[1]) ||
        !probe_tex(dev, cd->Width, cd->Height, cd->Format, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_pr.ring[0]) ||
        !probe_tex(dev, cd->Width, cd->Height, cd->Format, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_pr.ring[1]) ||
        FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)g_pr.rt_in, NULL, &g_pr.rtv_in)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)g_pr.rt_out, NULL, &g_pr.rtv_out))) {
        DLOG(log_s("probe: resource creation failed: probe off")); probe_release(); g_pr.failed = 1; return 0;
    }
    g_pr.rw = cd->Width; g_pr.rh = cd->Height; g_pr.fmt = cd->Format;
    DLOG(log_s("probe: ready (32x18 colour probe of input and output, full-frame ring for dumps)"));
    return 1;
}
static void probe_draw(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *rtv, UINT w, UINT h)
{
    float c[4]; D3D11_VIEWPORT vp; ID3D11ShaderResourceView *srv0[1] = { src }; ID3D11Buffer *cb0[1] = { g_pr.cb }; ID3D11RenderTargetView *rt[1] = { rtv };
    c[0] = (float)w; c[1] = (float)h; c[2] = c[3] = 0;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_pr.cb, 0, NULL, c, 0, 0);
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = PW; vp.Height = PH; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, rt, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffffu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_pr.vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0); ID3D11DeviceContext_HSSetShader(ctx, NULL, NULL, 0); ID3D11DeviceContext_DSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_pr.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, cb0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, srv0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    srv0[0] = NULL; ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, srv0);
    rt[0] = NULL; ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, rt, NULL);
}
/* blue-dominant fraction (percent) over the centre rows of a mapped 32x18 RGBA8 probe, plus mean RGB */
static int probe_stats(const BYTE *p, UINT pitch, int *mr, int *mg, int *mb)
{
    int x, y, blue = 0, n = 0; unsigned int sr = 0, sg = 0, sb = 0;
    for (y = PH * 3 / 10; y < PH * 88 / 100; y++) for (x = 0; x < PW; x++) {
        const BYTE *q = p + y * pitch + x * 4; int r = q[0], g = q[1], b = q[2], m = r > g ? r : g;
        sr += r; sg += g; sb += b; n++;
        if (b > 60 && b * 100 > m * 115) blue++;
    }
    if (!n) return 0;
    *mr = (int)(sr / n); *mg = (int)(sg / n); *mb = (int)(sb / n);
    return blue * 100 / n;
}
#endif /* DBG probe part 1 */
static void dump_ppm(ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, UINT w, UINT h, const WCHAR *tag, int bgra)   /* 8-bit RGBA/BGRA staging texture -> KCDDLSS_<tag>.ppm */
{
    D3D11_MAPPED_SUBRESOURCE m; WCHAR path[MAX_PATH + 48]; HANDLE f; char hdr[48], num[12]; DWORD wr; UINT y, x; BYTE *row; int r0 = bgra ? 2 : 0, b0 = bgra ? 0 : 2;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)tex, 0, D3D11_MAP_READ, 0, &m))) return;
    path[0] = 0; wcat(path, MAX_PATH + 48, g_dir); wcat(path, MAX_PATH + 48, L"\\KCDDLSS_"); wcat(path, MAX_PATH + 48, tag); wcat(path, MAX_PATH + 48, L".ppm");
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)tex, 0); return; }
    hdr[0] = 'P'; hdr[1] = '6'; hdr[2] = '\n'; hdr[3] = 0; utoa_w(num, w); lstrcatA(hdr, num); lstrcatA(hdr, " "); utoa_w(num, h); lstrcatA(hdr, num); lstrcatA(hdr, "\n255\n");
    WriteFile(f, hdr, lstrlenA(hdr), &wr, NULL);
    row = VirtualAlloc(NULL, w * 3, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (row) for (y = 0; y < h; y++) {
        const BYTE *src = (const BYTE *)m.pData + y * m.RowPitch;
        for (x = 0; x < w; x++) { row[x * 3] = src[x * 4 + r0]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4 + b0]; }
        WriteFile(f, row, w * 3, &wr, NULL);
    }
    if (row) VirtualFree(row, 0, MEM_RELEASE);
    CloseHandle(f);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)tex, 0);
    LOG(log_s("dump: wrote "); log_w(path); log_s(" ("); log_u(w); log_s("x"); log_u(h); log_s(")"));
}
/* `dump` console command (0.6.0): the DLSS colour input (render size) and our output (native size) as PPM, next to the plugin */
static void dump_frame(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *col_res, const D3D11_TEXTURE2D_DESC *cd)
{
    ID3D11Texture2D *st = NULL; D3D11_TEXTURE2D_DESC od; int bgra;
    g_dump_req = 0;
    bgra = (cd->Format == 87 || cd->Format == 88 || cd->Format == 90 || cd->Format == 91);   /* B8G8R8A8 UNORM/UNORM_SRGB, TYPELESS */
    if (!(cd->Format == 27 || cd->Format == 28 || cd->Format == 29 || bgra)) {   /* 27 = R8G8B8A8_TYPELESS (the game's colour target) */ LOG(log_s("dump: colour format "); log_u(cd->Format); log_s(" is not 8-bit RGBA/BGRA: not dumped")); return; }
    if (probe_tex(dev, cd->Width, cd->Height, cd->Format, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &st)) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, col_res); dump_ppm(ctx, st, cd->Width, cd->Height, L"in", bgra); REL(st);
    } else LOG(log_s("dump: staging texture for the input failed"));
    if (g_dl.out && tex_desc((ID3D11Resource *)g_dl.out, &od) && probe_tex(dev, od.Width, od.Height, od.Format, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &st)) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)g_dl.out); dump_ppm(ctx, st, od.Width, od.Height, L"out", od.Format == 87 || od.Format == 91); REL(st);
    } else LOG(log_s("dump: staging texture for the output failed"));
}
#if DBG
/* called inside the saved-state window, after EvaluateFeature */
static void probe_frame(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *csrv, ID3D11Resource *col_res, const D3D11_TEXTURE2D_DESC *cd)
{
    unsigned int cur, prev; D3D11_MAPPED_SUBRESOURCE mi, mo; int bi, bo, r1, g1, b1, r2, g2, b2;
    if (!g_cfg.probe || !probe_init(dev, cd)) return;
    if (g_pr.out_of != g_dl.out) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd; memset(&sd, 0, sizeof sd);
        sd.Format = uav_format(g_dl.out_fmt); sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        REL(g_pr.out_srv);
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_dl.out, &sd, &g_pr.out_srv))) { g_pr.out_srv = NULL; return; }
        g_pr.out_of = g_dl.out;
    }
    cur = g_pr.frames & 1; prev = cur ^ 1;
    probe_draw(ctx, csrv, g_pr.rtv_in, cd->Width, cd->Height);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)g_pr.st_in[cur], (ID3D11Resource *)g_pr.rt_in);
    probe_draw(ctx, g_pr.out_srv, g_pr.rtv_out, g_dl.nw, g_dl.nh);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)g_pr.st_out[cur], (ID3D11Resource *)g_pr.rt_out);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)g_pr.ring[cur], col_res);
    if (g_pr.frames > 0) {
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_pr.st_in[prev], 0, D3D11_MAP_READ, 0, &mi))) {
            bi = probe_stats((const BYTE *)mi.pData, mi.RowPitch, &r1, &g1, &b1);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_pr.st_in[prev], 0);
            bo = -1; r2 = g2 = b2 = 0;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_pr.st_out[prev], 0, D3D11_MAP_READ, 0, &mo))) {
                bo = probe_stats((const BYTE *)mo.pData, mo.RowPitch, &r2, &g2, &b2);
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_pr.st_out[prev], 0);
            }
            /* a one-frame glitch = the input jumps away from the previous frame (blue fraction or mean colour) and the
             * previous frame itself was not flagged (so loading screens / sky shots do not count) */
            { int dblue = bi - g_pr.pbi, dsum = (r1 - g_pr.pr) + (g1 - g_pr.pg) + (b1 - g_pr.pb), hue = (b1 - r1) - (g_pr.pb - g_pr.pr), pbi0 = g_pr.pbi;
              int spike = g_pr.frames > 1 && !g_pr.pflag && (dblue >= 12 || dblue <= -12 || hue >= 12 || hue <= -12 || dsum >= 60 || dsum <= -60);
              unsigned int hi = g_pr.hn & 15, k;
              g_pr.pflag = spike; g_pr.pbi = bi; g_pr.pr = r1; g_pr.pg = g1; g_pr.pb = b1;
              g_pr.hist[hi][0] = bi; g_pr.hist[hi][1] = r1; g_pr.hist[hi][2] = g1; g_pr.hist[hi][3] = b1; g_pr.hn++;
              if (g_pr.after > 0 && --g_pr.after == 0) {   /* 8 frames after a spike: print them, oldest first */
                  DLOG(log_s("probe: after spike, frames "); log_u(g_dl.frames - 8); log_s(".."); log_u(g_dl.frames - 1); log_s(" input r/g/b:");
                      for (k = 8; k >= 1; k--) { int *e = g_pr.hist[(g_pr.hn - k) & 15]; log_s(" "); log_i(e[1]); log_s("/"); log_i(e[2]); log_s("/"); log_i(e[3]); });
              }
            if (spike && g_pr.logs < 40) {
                g_pr.logs++; g_pr.after = 8;
                DLOG(log_s("probe: SPIKE frame "); log_u(g_dl.frames - 1); log_s(": prev input blue "); log_i(pbi0); log_s("% -> input blue "); log_i(bi); log_s("% mean "); log_i(r1); log_s("/"); log_i(g1); log_s("/"); log_i(b1);
                    log_s(", output blue "); log_i(bo); log_s("% mean "); log_i(r2); log_s("/"); log_i(g2); log_s("/"); log_i(b2); log_s(", reset "); log_i(g_dl.reset); log_s("; previous 7 frames r/g/b:");
                    for (k = 8; k >= 2; k--) { int *e = g_pr.hist[(g_pr.hn - k) & 15]; log_s(" "); log_i(e[1]); log_s("/"); log_i(e[2]); log_s("/"); log_i(e[3]); }
                    log_s("; bind masks last 8 hooks (spike frame = 2nd last):"); for (k = 8; k >= 1; k--) { log_s(" "); log_x(g_bmask[(g_bmask_n - k) & 15]); });
                DLOG(log_s("postfx: game FX state last 4 hooks (spike frame = 2nd last):"); for (k = 4; k >= 1; k--) fx_log_state(k));
                if (g_pr.dumps < 4 && g_dl.reset == 0) { WCHAR tag[8] = L"blue_0"; g_pr.dumps++; tag[5] = (WCHAR)('0' + g_pr.dumps % 10); dump_ppm(ctx, g_pr.ring[prev], cd->Width, cd->Height, tag, 0); }
            } else if (g_pr.frames == 2 || (g_pr.frames % 600) == 0) {
                DLOG(log_s("probe: frame "); log_u(g_dl.frames - 1); log_s(" input blue "); log_i(bi); log_s("% mean "); log_i(r1); log_s("/"); log_i(g1); log_s("/"); log_i(b1);
                    log_s(", output blue "); log_i(bo); log_s("% mean "); log_i(r2); log_s("/"); log_i(g2); log_s("/"); log_i(b2); log_s(", game FX"); if (g_fxh_n) fx_log_state(1); if (g_fx_wrapped >= 0) { log_s(" wrapped Render calls "); log_u(g_fx_wrap_calls); });
            } }
        }
    }
    g_pr.frames++;
}
#else
#define probe_frame(dev, ctx, csrv, col_res, cd) ((void)0)
#endif

/* ---- 4x4 row-major float matrices (CryEngine Matrix44 convention: row vector * matrix) ---- */
static void m4_mul(float *r, const float *a, const float *b)
{
    int i, j, k;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) { float s = 0; for (k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; }
}
static int m4_inverse(float *out, const float *m)   /* general inverse via cofactors, double precision */
{
    double inv[16], det; int i;
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-30 && det < 1e-30) return 0;
    det = 1.0 / det;
    for (i = 0; i < 16; i++) out[i] = (float)(inv[i] * det);
    return 1;
}

/* Reprojection for the shader: row vector (uv.x, uv.y, depth, 1) * M -> previous clip position (divide by w = prev uv).
 * M = uvToNdc * inverse(viewProjCur) * viewProjPrev * ndcToUv, all row-major, both matrices WITHOUT jitter. */
static void build_reprojection(float *M, const float *viewProjCur, const float *viewProjPrev)
{
    static const float uvToNdc[16] = { 2,0,0,0,  0,-2,0,0,  0,0,1,0,  -1,1,0,1 };
    static const float ndcToUv[16] = { 0.5f,0,0,0,  0,-0.5f,0,0,  0,0,1,0,  0.5f,0.5f,0,1 };
    float inv[16], t1[16], t2[16];
    if (!m4_inverse(inv, viewProjCur)) { memset(M, 0, 64); M[0] = M[5] = M[10] = M[15] = 1; return; }   /* identity: zero motion */
    m4_mul(t1, uvToNdc, inv); m4_mul(t2, t1, viewProjPrev); m4_mul(M, t2, ndcToUv);
}

/* Camera matrices (static analysis 2026-11-08, agent report in WORKING_NOTES): at post-processing begin the game
 * snapshots the current view matrix to WHGame+0x34b95b8 and the UNJITTERED projection to WHGame+0x34b95f8 (both
 * row-major 4x4, row-vector convention). Its own "previous view" slot (renderer+0xaa0) is already overwritten with this
 * frame's view by the time the upscale runs, so we keep our own previous view AND previous projection (the game
 * ignores FOV changes; we do not). Returns 0 (zero motion, DLSS reset) on the first frame or after a camera cut. */
#define RVA_POST_VIEW (g_wh_build->post_view)
#define RVA_POST_PROJ (g_wh_build->post_proj)
static float g_prev_view[16], g_prev_proj[16], g_prev_cam[3]; static int g_have_prev;
static void cam_pos(const float *V, float *c)
{
    int k;
    for (k = 0; k < 3; k++) c[k] = -(V[12] * V[k * 4 + 0] + V[13] * V[k * 4 + 1] + V[14] * V[k * 4 + 2]);
}
static int get_view_proj(BYTE *self, float *viewProjCur, float *viewProjPrev)
{
    const float *V = (const float *)(g_wh_base + RVA_POST_VIEW), *P = (const float *)(g_wh_base + RVA_POST_PROJ);
    float dx, dy, dz, jump, cur[3]; int ok = 0;
    (void)self;
    if (!readable((const BYTE *)V, 128)) return 0;
    if (P[0] == 0 || P[5] == 0 || P[14] == 0) { if (g_dl.frames < 2) LOG(log_s("mv: projection snapshot not a perspective matrix (p00/p11/p32 zero): zero vectors")); return 0; }
    cam_pos(V, cur);
    if (g_dl.frames < 2) LOG(log_s("mv: proj p00 "); log_i((int)(P[0] * 1000)); log_s(" p11 "); log_i((int)(P[5] * 1000)); log_s(" p22 "); log_i((int)(P[10] * 1000)); log_s(" p23 "); log_i((int)(P[11] * 1000));
                             log_s(" p32 "); log_i((int)(P[14] * 1000)); log_s(" (milli); view row3 "); log_i((int)V[12]); log_s(" "); log_i((int)V[13]); log_s(" "); log_i((int)V[14]); log_s(", camera "); log_i((int)cur[0]); log_s(" "); log_i((int)cur[1]); log_s(" "); log_i((int)cur[2]));
    /* camera world position c = -t * R^T (row-vector view matrix: p_view = p_world * R + t). The raw translation row
     * is -c*R and swings with every rotation when the camera is kilometres from the origin (0.3.1 bug: a reset per turn). */
    if (g_have_prev) {
        static unsigned int cuts;
        dx = cur[0] - g_prev_cam[0]; dy = cur[1] - g_prev_cam[1]; dz = cur[2] - g_prev_cam[2]; jump = dx * dx + dy * dy + dz * dz;
        if (jump < 25.0f * 25.0f) {                       /* a jump of 25 m or more in one frame = camera cut */
            m4_mul(viewProjCur, V, P); m4_mul(viewProjPrev, g_prev_view, g_prev_proj); ok = 1;
        } else { g_dl.reset = 1; g_dl.hold = 30; if (++cuts <= 6) LOG(log_s("mv: camera cut ("); log_i((int)cur[0]); log_s(" "); log_i((int)cur[1]); log_s(" "); log_i((int)cur[2]); log_s(" from ");
                                                       log_i((int)g_prev_cam[0]); log_s(" "); log_i((int)g_prev_cam[1]); log_s(" "); log_i((int)g_prev_cam[2]); log_s("): DLSS reset")); }
    } else { g_dl.reset = 1; g_dl.hold = 30; }
    memcpy(g_prev_cam, cur, sizeof cur);
    memcpy(g_prev_view, V, 64); memcpy(g_prev_proj, P, 64); g_have_prev = 1;
    return ok;
}

static void cvars_dump_once(void);
static void cvars_exec_once(void);
static void cmdfile_poll(void);
static int kick_activate(const char *why);   /* 0.8.0: the renderer re-activate repair, defined with the menu code below */
static void fx_set(const char *name, int mode, const char *src);
static void mode_apply(int mode, const char *src);
static void dlssmode_sync(int mode);
static void dlsspreset_sync(void);
static void *cvar_find(const char *name);
static int g_startup_done;
static UINT g_mode_rw, g_mode_rh;   /* render size the current mode asked for (watchdog) */
static UINT g_mode_nw, g_mode_nh;   /* output size that render size was computed from */
static UINT g_true_nw, g_true_nh;   /* 0.7.1: the real output size = the native-size target the stretch hook saw while upscaling. The
                                     * renderer's native fields lag one frame behind a r_FullscreenNativeRes flip (run 064 start: 1920x1080
                                     * read, 3840x2160 real -> 960x540 render), and the settings menu's Apply copies r_Width (our render
                                     * size) into the output size (run 064 crash: 1280x720 output, DLSS feature for 4K, DEVICE_REMOVED). */
/* Stale-binding check (FINDINGS_KCD_COLORGRADING.md): CTexture::Apply skips the D3D bind when its per-slot record at
 * WHGame+0x33ff908 + slot*0x40 (+0x28 SRV) already matches. Compare the record with the live PS SRVs at hook time; a
 * frame where they differ (record non-null, live different) gets a bit per slot. Masks kept for the last 16 frames. */
#define RVA_BINDREC (g_wh_build->bindrec)
static void bind_check(ID3D11DeviceContext *ctx)
{
    ID3D11ShaderResourceView *live[16]; unsigned int m = 0, i; BYTE *rec = g_wh_base + RVA_BINDREC;
    if (!readable(rec, 16 * 0x40)) { g_bmask[g_bmask_n++ & 15] = 0x80000000u; return; }
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 16, live);
    for (i = 0; i < 16; i++) {
        void *r = *(void **)(rec + i * 0x40 + 0x28);
        if (r && r != (void *)live[i]) m |= 1u << i;
        if (live[i]) ID3D11ShaderResourceView_Release(live[i]);
    }
    g_bmask[g_bmask_n++ & 15] = m;
    if (!g_bind_logged && g_bmask_n == 120) { g_bind_logged = 1; DLOG(log_s("bind: record vs live PS SRV mismatch mask at frame 120: "); log_x(m); log_s(" (0 = engine cache consistent at hook time)")); }
}

/* ---- game post effects (FINDINGS_KCD_POSTFX.md) ----
 * CPostEffectsMgr* = *(renderer+0xef68); its std::vector<CPostEffect*> at +0xa8/+0xb0 is indexed by effect id.
 * CPostEffect: +0x09 id, vtable slot 4 Preprocess (bool), 11 GetName; CEffectParam* at the effect's own offsets,
 * CEffectParam vtable slot 7 GetParam (float), slot 8 GetParamVec4(this, float out[4]). The 8 effects below only run
 * when r_PostProcessGameFx (int at WHGame+0x3786b84) is non-zero; they are sampled at every hook call (render thread,
 * after CREPostProcess::mfDraw) and the last 16 states are printed on probe spikes. "fxoff Name" swaps the effect's
 * vptr to a private copy whose Preprocess always returns false; "fxon Name" restores it. */
#define PFX_MGR_OFF 0xef68
#define RVA_GAMEFX (g_wh_build->gamefx)
#define RVA_CUSTOMVIS (g_wh_build->customvis)
#define FX_VT_SLOTS 24
typedef const char *(*PFN_FxName)(void *fx);
typedef float (*PFN_ParamF)(void *p);
typedef float *(*PFN_ParamV)(void *p, float *out);
static const struct { const char *name; int id; int off; int thr; } g_gated[8] = {
    { "WaterRipples", 0x01, 0x28, 5 }, { "HUDSilhouettes", 0x07, 0x30, 5 }, { "VolumetricScattering", 0x0c, 0x18, 5 }, { "WaterFlow", 0x18, 0x18, 5 },
    { "ScreenBlood", 0x19, 0x18, 5 }, { "ScreenFrost", 0x1a, 0x18, 90 }, { "AlienInterference", 0x1c, 0x18, 90 }, { "GhostVision", 0x1d, 0x40, 90 } };
static struct { void *fx; const char *name; int id; void **orig_vt; void *vt_copy[FX_VT_SLOTS]; int off; } g_fx[40];
static int g_fx_n, g_fx_state;   /* state: 0 not scanned, 1 ok, -1 failed (never retried) */
static int g_fx_wrapped_init;   /* g_fx_wrapped is forced to -1 on the first fx_scan() */
static void *g_fx_gated[8];
static struct { int gamefx, amt[8], alien[4], vol[4], hud_type, hud_fill; } g_fxh[16];
static BYTE *g_renderer;
static int fx_preprocess_false(void *self) { (void)self; return 0; }
static float fx_param(void *fx, int off)   /* -1 when the param pointer is missing */
{
    void *p;
    if (!fx || !readable((BYTE *)fx + off, 8)) return -1.0f;
    p = *(void **)((BYTE *)fx + off);
    if (!p || !readable((BYTE *)p, 8) || !readable(*(BYTE **)p, 9 * 8)) return -1.0f;
    return ((PFN_ParamF)VT(p, 7))(p);
}
static void fx_param4(void *fx, int off, int *out)
{
    void *p; float v[4] = { 0, 0, 0, 0 }; int i;
    out[0] = out[1] = out[2] = out[3] = -1;
    if (!fx || !readable((BYTE *)fx + off, 8)) return;
    p = *(void **)((BYTE *)fx + off);
    if (!p || !readable((BYTE *)p, 8) || !readable(*(BYTE **)p, 9 * 8)) return;
    ((PFN_ParamV)VT(p, 8))(p, v);
    for (i = 0; i < 4; i++) out[i] = (int)(v[i] * 1000.0f);
}
static int fx_scan(void)
{
    BYTE *mgr, **b, **e; int i, k, n;
    if (!g_fx_wrapped_init) { g_fx_wrapped_init = 1; g_fx_wrapped = -1; }
    if (g_fx_state) return g_fx_state > 0;
    if (!g_renderer || !readable(g_renderer + PFX_MGR_OFF, 8)) return 0;
    mgr = *(BYTE **)(g_renderer + PFX_MGR_OFF);
    if (!mgr || !readable(mgr + 0xa8, 16)) return 0;   /* not created yet: retry */
    b = *(BYTE ***)(mgr + 0xa8); e = *(BYTE ***)(mgr + 0xb0);
    n = (int)(e - b);
    if (!b || n < 8 || n > 40 || !readable((BYTE *)b, (SIZE_T)n * 8)) { g_fx_state = -1; DLOG(log_s("postfx: effect vector not recognised ("); log_i(n); log_s(" entries), post-effect tools off")); return 0; }
    for (i = 0; i < n; i++) {
        BYTE *fx = b[i]; const char *nm;
        g_fx[i].fx = fx; g_fx[i].name = "?"; g_fx[i].id = -1;
        if (!fx || !readable(fx, 16) || !readable(*(BYTE **)fx, 13 * 8)) continue;
        g_fx[i].id = fx[9];
        nm = ((PFN_FxName)VT(fx, 11))(fx);
        if (nm && readable((const BYTE *)nm, 32)) g_fx[i].name = nm;
    }
    g_fx_n = n;
    for (k = 0; k < 8; k++) {
        g_fx_gated[k] = NULL;
        for (i = 0; i < n; i++) if (g_fx[i].id == g_gated[k].id && streq(g_fx[i].name, g_gated[k].name)) g_fx_gated[k] = g_fx[i].fx;
    }
    g_fx_state = 1;
    DLOG(log_s("postfx: "); log_i(n); log_s(" effects:"); for (i = 0; i < n; i++) { log_s(" "); log_i(g_fx[i].id); log_s(":"); log_s(g_fx[i].name); });
    DLOG(log_s("postfx: gated effects found:"); for (k = 0; k < 8; k++) { log_s(" "); log_s(g_gated[k].name); log_s(g_fx_gated[k] ? "=ok" : "=MISSING"); }
        log_s("; r_PostProcessGameFx="); log_i(readable(g_wh_base + RVA_GAMEFX, 4) ? *(int *)(g_wh_base + RVA_GAMEFX) : -1));
    return 1;
}
static void fx_sample(void)
{
    unsigned int h = g_fxh_n & 15; int k;
    if (!fx_scan()) return;
    g_fxh[h].gamefx = readable(g_wh_base + RVA_GAMEFX, 4) ? *(int *)(g_wh_base + RVA_GAMEFX) : -1;
    for (k = 0; k < 8; k++) {
        float a = fx_param(g_fx_gated[k], g_gated[k].off);
        if (k == 7) { float a2 = fx_param(g_fx_gated[k], 0x48), a3 = fx_param(g_fx_gated[k], 0x50); if (a2 > a) a = a2; if (a3 > a) a = a3; }
        g_fxh[h].amt[k] = (int)(a * 1000.0f);
    }
    fx_param4(g_fx_gated[6], 0x20, g_fxh[h].alien);
    fx_param4(g_fx_gated[2], 0x30, g_fxh[h].vol);
    g_fxh[h].hud_type = (int)(fx_param(g_fx_gated[1], 0x40) * 1000.0f);
    g_fxh[h].hud_fill = (int)(fx_param(g_fx_gated[1], 0x38) * 1000.0f);
    g_fxh_n++;
}
/* one frame's state as text: gamefx value, then every gated effect whose amount is above its threshold (x1000) */
#if DBG
static void fx_log_state(unsigned int back)
{
    int k, any = 0; unsigned int h = (g_fxh_n - back) & 15;
    log_s(" [gamefx="); log_i(g_fxh[h].gamefx);
    for (k = 0; k < 8; k++) if (g_fxh[h].amt[k] > g_gated[k].thr) {
        any = 1; log_s(" "); log_s(g_gated[k].name); log_s("="); log_i(g_fxh[h].amt[k]);
        if (k == 6) { log_s(" clr "); log_i(g_fxh[h].alien[0]); log_s("/"); log_i(g_fxh[h].alien[1]); log_s("/"); log_i(g_fxh[h].alien[2]); }
        if (k == 2) { log_s(" clr "); log_i(g_fxh[h].vol[0]); log_s("/"); log_i(g_fxh[h].vol[1]); log_s("/"); log_i(g_fxh[h].vol[2]); }
        if (k == 1) { log_s(" type "); log_i(g_fxh[h].hud_type); log_s(" fill "); log_i(g_fxh[h].hud_fill); }
    }
    if (!any) log_s(" none");
    log_s("]");
}
#endif
/* fxwrap: keep the effect, but after its Render restore the renderer's state (FINDINGS_KCD_WATERRIPPLES.md: the ripple hit
 * pass sets state 0x4b022 = additive blend, blue-only colour write, and never restores it; FX_SetState = renderer vtable
 * +0xc98, (R, state, alpharef -1, 0); the other passes use 0x40000). One wrapped effect at a time. */
#define FX_SETSTATE_SLOT (0xc98 / 8)
#define FX_STATE_DEFAULT 0x40000
typedef void (*PFN_FxRender)(void *fx);
typedef void (*PFN_FxSetState)(void *R, int state, int alpharef, int restore);
static void fx_render_wrap(void *fx)
{
    if (g_fx_wrapped >= 0 && g_fx[g_fx_wrapped].orig_vt) ((PFN_FxRender)g_fx[g_fx_wrapped].orig_vt[6])(fx);
    if (g_renderer) ((PFN_FxSetState)VT(g_renderer, FX_SETSTATE_SLOT))(g_renderer, FX_STATE_DEFAULT, -1, 0);
    g_fx_wrap_calls++;
}
/* mode 0 = restore the original vtable, 1 = Preprocess -> false (effect off), 2 = wrap Render (state restored after it) */
static int g_fx_pending;   /* 0.6.7: an fx command came before the post effects existed: the frame tick retries the ini Exec */
static void fx_set(const char *name, int mode, const char *src)
{
    int i, k;
    if (!fx_scan()) { g_fx_pending = 1; LOG(log_s("exec("); log_s(src); log_s("): post effects not ready, fx command for "); log_s(name); log_s(" skipped")); return; }
    for (i = 0; i < g_fx_n; i++) if (g_fx[i].fx && streq(g_fx[i].name, name)) break;
    if (i == g_fx_n) { LOG(log_s("exec("); log_s(src); log_s("): unknown post effect "); log_s(name)); return; }
    if (mode) {
        void **vt = g_fx[i].orig_vt ? g_fx[i].orig_vt : *(void ***)g_fx[i].fx;
        if (mode == 2 && g_fx_wrapped >= 0 && g_fx_wrapped != i) { LOG(log_s("exec("); log_s(src); log_s("): another effect is already wrapped, fxon it first")); return; }
        if (!readable((BYTE *)vt, FX_VT_SLOTS * 8)) { LOG(log_s("exec("); log_s(src); log_s("): vtable of "); log_s(name); log_s(" not readable")); return; }
        if (mode == 2 && (!g_renderer || !readable(*(BYTE **)g_renderer + FX_SETSTATE_SLOT * 8, 8))) { LOG(log_s("exec("); log_s(src); log_s("): renderer vtable not readable")); return; }
        for (k = 0; k < FX_VT_SLOTS; k++) g_fx[i].vt_copy[k] = vt[k];
        if (mode == 1) g_fx[i].vt_copy[4] = (void *)fx_preprocess_false;
        else { g_fx_wrapped = i; g_fx[i].vt_copy[6] = (void *)fx_render_wrap; }
        g_fx[i].orig_vt = vt;
        *(void ***)g_fx[i].fx = g_fx[i].vt_copy;
        LOG(log_s("exec("); log_s(src); log_s("): post effect "); log_s(name); log_s(mode == 1 ? " OFF (Preprocess -> false)" : " WRAPPED (Render, then FX_SetState 0x40000)"));
    } else {
        if (!g_fx[i].orig_vt) { LOG(log_s("exec("); log_s(src); log_s("): "); log_s(name); log_s(" was not off/wrapped")); return; }
        *(void ***)g_fx[i].fx = g_fx[i].orig_vt; g_fx[i].orig_vt = NULL;
        if (g_fx_wrapped == i) g_fx_wrapped = -1;
        LOG(log_s("exec("); log_s(src); log_s("): post effect "); log_s(name); log_s(" ON (vtable restored), wrapped Render calls so far "); log_u(g_fx_wrap_calls));
    }
}
static void dlss_frame(BYTE *self)
{
    ID3D11ShaderResourceView *dsrv, *csrv = NULL; ID3D11RenderTargetView *rtv = NULL;
    ID3D11Resource *out_res = NULL, *col_res = NULL, *dep_res = NULL; ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    D3D11_TEXTURE2D_DESC od, cd, dd; CtxState st; NGX_Result r; float dsx, dsy, sx, sy;
    UINT rw = *(UINT *)(self + R_RENDER_W), rh = *(UINT *)(self + R_RENDER_H), nw = *(UINT *)(self + R_NATIVE_W), nh = *(UINT *)(self + R_NATIVE_H);
    dsrv = depth_view();
    if (!dsrv) { dl_fail("hardware depth view not found"); return; }
    ID3D11ShaderResourceView_GetDevice(dsrv, &dev);
    if (!dev) { REL(dsrv); dl_fail("no device from the depth view"); return; }
    ID3D11Device_GetImmediateContext(dev, &ctx);
    hook_ctx(ctx);
    g_sc_bias = mip_bias_now();
    if (g_sc_bias != g_css_last_bias) { g_css_last_bias = g_sc_bias; LOG(log_s("mip bias now "); log_i((int)(g_sc_bias * 1000)); log_s(" milli = log2(render/native) + offset "); log_i((int)(g_cfg.mipoff * 1000)); log_s(" milli; sampler cache "); log_i(g_sc_n); log_s(" entries, "); log_u(g_sc_subst); log_s(" biased binds so far")); }
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_PSGetShaderResources(ctx, 0, 1, &csrv);
    bind_check(ctx);
    g_renderer = self; fx_sample();
    if (rtv) ID3D11RenderTargetView_GetResource(rtv, &out_res);
    if (csrv) ID3D11ShaderResourceView_GetResource(csrv, &col_res);
    ID3D11ShaderResourceView_GetResource(dsrv, &dep_res);
    if (!tex_desc(out_res, &od)) dl_fail("no render target bound after the stretch");
    else if (!tex_desc(col_res, &cd)) dl_fail("no colour texture at pixel-shader slot 0 after the stretch");
    else if (!tex_desc(dep_res, &dd)) dl_fail("depth view has no 2D texture");
    else {
        if (g_dl.frames < 2) LOG(log_s("dlss: frame "); log_u(g_dl.frames); log_s(": colour "); log_u(cd.Width); log_s("x"); log_u(cd.Height); log_s(" fmt "); log_u(cd.Format);
                                 log_s(", depth "); log_u(dd.Width); log_s("x"); log_u(dd.Height); log_s(" fmt "); log_u(dd.Format);
                                 log_s(", target "); log_u(od.Width); log_s("x"); log_u(od.Height); log_s(" fmt "); log_u(od.Format);
                                 log_s(", renderer "); log_u(rw); log_s("x"); log_u(rh); log_s(" -> "); log_u(nw); log_s("x"); log_u(nh);
                                 log_s(", AA mode "); log_i(*(int *)(g_wh_base + RVA_AA_MODE)));
        {   /* diagnostic (blue flash on save load, user 2026-11-08): does the stretch input change identity? */
            static ID3D11Resource *last_col, *last_out; static unsigned int changes;
            if ((col_res != last_col || out_res != last_out) && g_dl.frames > 0 && changes < 12) {
                changes++;
                LOG(log_s("dlss: frame "); log_u(g_dl.frames); log_s(": input changed: colour "); log_x((ULONG_PTR)col_res); log_s(" fmt "); log_u(cd.Format); log_s(" "); log_u(cd.Width); log_s("x"); log_u(cd.Height);
                    log_s(" bind "); log_x(cd.BindFlags); log_s(", target "); log_x((ULONG_PTR)out_res); log_s(" fmt "); log_u(od.Format); log_s(", depth "); log_x((ULONG_PTR)dep_res); log_s(", reset "); log_i(g_dl.reset));
            }
            last_col = col_res; last_out = out_res;
        }
        if (od.Width == nw && od.Height == nh && cd.Width == rw && cd.Height == rh && (od.Width != cd.Width || od.Height != cd.Height)) {
            g_true_nw = od.Width; g_true_nh = od.Height;   /* 0.7.1: the real output size (see g_true_nw) */
            if (g_startup_done && g_cfg.mode >= 0 && g_mode_rw && (g_mode_nw != g_true_nw || g_mode_nh != g_true_nh)) {
                static DWORD last_fix; DWORD now = GetTickCount();
                if ((int)(now - last_fix) > 1000) {
                    last_fix = now;
                    LOG(log_s("dlss: output is "); log_u(g_true_nw); log_s("x"); log_u(g_true_nh); log_s(" but the mode was computed for "); log_u(g_mode_nw); log_s("x"); log_u(g_mode_nh); log_s(": re-applying"));
                    mode_apply(g_cfg.mode, "resize");
                }
            }
        }
        if (od.Width != nw || od.Height != nh) dl_fail("render target is not the native size");
        else if (cd.Width != rw || cd.Height != rh) dl_fail("colour texture is not the render size");
        else if (dd.Width != rw || dd.Height != rh) dl_fail("depth texture is not the render size");
        else if (od.SampleDesc.Count != 1 || cd.SampleDesc.Count != 1) dl_fail("multisampled target");
        else if (ensure_feature(dev, ctx, rw, rh, nw, nh, od.Format)) {
            NGX_Parameter *p = g_dl.params;
            p_set_p(p, "Color", col_res); p_set_p(p, "Output", (ID3D11Resource *)g_dl.out);
            p_set_p(p, "Depth", dep_res); p_set_p(p, "MotionVectors", (ID3D11Resource *)g_dl.mv);
            p_set_f(p, "Jitter.Offset.X", -g_dl.jx); p_set_f(p, "Jitter.Offset.Y", g_dl.jy);
            p_set_f(p, "MV.Scale.X", 1.0f); p_set_f(p, "MV.Scale.Y", 1.0f);
            p_set_i(p, "Reset", g_dl.reset);
            p_set_f(p, "Sharpness", g_cfg.sharpness);
            p_set_u(p, "DLSS.Render.Subrect.Dimensions.Width", rw); p_set_u(p, "DLSS.Render.Subrect.Dimensions.Height", rh);
            p_set_f(p, "DLSS.Exposure.Scale", 1.0f); p_set_f(p, "DLSS.Pre.Exposure", 1.0f);
            ctx_save(ctx, &st);
            { float vpc[16], vpp[16], M[16]; UINT vw = 0, vh = 0; ID3D11ShaderResourceView *vsrv = g_cfg.objectmv ? velocity_view(&vw, &vh) : NULL;
              if (vsrv && (vw != rw || vh != rh) && g_dl.frames < 2) LOG(log_s("velocity: buffer "); log_u(vw); log_s("x"); log_u(vh); log_s(" differs from the render size: sampled by uv"));
              if (g_dl.frames < 2) LOG(log_s("dlss: object motion vectors "); log_s(vsrv ? "on" : "off (no velocity view)"));
              if (get_view_proj(self, vpc, vpp)) { build_reprojection(M, vpc, vpp); mv_pass(dev, ctx, dsrv, vsrv, vw, vh, M, rw, rh); }
              REL(vsrv); }
            r = g_ngx.EvaluateFeature(ctx, g_dl.feature, p, NULL);
            if (r == NGX_OK) probe_frame(dev, ctx, csrv, col_res, &cd);
            if (r == NGX_OK && g_dump_req) dump_frame(dev, ctx, col_res, &cd);
            ctx_restore(ctx, &st);
            if (r == NGX_OK) {
                ID3D11DeviceContext_CopyResource(ctx, out_res, (ID3D11Resource *)g_dl.out);
                if (g_dl.hold > 0) g_dl.hold--; else g_dl.reset = 0;   /* keep resetting ~0.5 s after a cut/load (stale history trace) */
                if (g_dl.frames < 2) LOG(log_s("dlss: frame "); log_u(g_dl.frames); log_s(" evaluated and copied to the target; samplers so far "); log_u(g_css_total); log_s(" ("); log_u(g_css_biased); log_s(" biased), jitter "); log_i((int)(g_dl.jx * 1000)); log_s("/"); log_i((int)(g_dl.jy * 1000)); log_s(" mpx"));
            } else {
                dl_fail("EvaluateFeature failed (result logged next)");
                if (g_dl.errs <= 6) LOG(log_s("dlss: result "); log_x(r));
            }
            g_dl.frames++;
            if (g_dl.frames == 2) cvars_dump_once();
            if (g_dl.frames >= 3) cvars_exec_once();
            if ((g_dl.frames % 30) == 0) cmdfile_poll();
            /* jitter for the NEXT frame: pixels -> the renderer's NDC float2 */
            g_dl.phase = (g_dl.phase + 1) % g_dl.phases;
            sx = halton((unsigned)g_dl.phase + 1, 2) - 0.5f; sy = halton((unsigned)g_dl.phase + 1, 3) - 0.5f;
            dsx = *(float *)(self + R_DOWNSCALE_X); dsy = *(float *)(self + R_DOWNSCALE_Y);
            if (!(dsx > 0.01f && dsx < 100.0f)) dsx = 1.0f;
            if (!(dsy > 0.01f && dsy < 100.0f)) dsy = 1.0f;
            *(float *)(self + R_JITTER_X) = 2.0f * sx / (float)rw / dsx;
            *(float *)(self + R_JITTER_Y) = 2.0f * sy / (float)rh / dsy;
            g_dl.jx = sx; g_dl.jy = sy;
        }
    }
    { DWORD now = GetTickCount(); int blink = (g_blink_end && (int)(g_blink_end - now) > 0 && ((now / 2000) & 1)) || (g_black_end && (int)(g_black_end - now) > 0);
      if ((g_black > 0 || blink) && rtv) { static const float black[4] = { 0, 0, 0, 1 }; ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black); if (g_black > 0) g_black--; } }
    REL(out_res); REL(col_res); REL(dep_res); REL(rtv); REL(csrv); REL(dsrv); REL(ctx); REL(dev);
}

/* ---- Mip bias: log2(renderW/nativeW) + offset. Vendor guidance (checked 2026-11-08): NVIDIA DLSS Programming Guide 3.5
 * "log2(RenderX/DisplayX) - 1.0 + epsilon" (never above plain log2), AMD FSR 2/3 "log2(render/display) - 1.0", Intel XeSS
 * "log2(input/target)" (more is optional). Default offset -1.0 (KCDDLSS.ini MipBiasOffset, console `mipbias <offset>`).
 * KCD has no mip bias of its own. 0.6.0: the bias is applied when the game BINDS samplers (context vtable hook, all stages)
 * from a cache of biased twins, so a live mode change re-biases everything at once; the CreateSamplerState hook only counts. */
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSamplerState)(ID3D11Device *, const D3D11_SAMPLER_DESC *, ID3D11SamplerState **);
static PFN_CreateSamplerState g_real_css;
static void **g_css_vtable;
static float g_css_last_bias;
#define MIP_EPSILON (-0.5f)

static float log2f_approx(float x)   /* x > 0; exponent from the bits + series on the mantissa, error < 1e-4 */
{
    union { float f; unsigned int u; } v; int e; float m, t, t2, s;
    v.f = x; e = (int)((v.u >> 23) & 0xff) - 127; v.u = (v.u & 0x007fffffu) | 0x3f800000u; m = v.f;   /* m in [1,2) */
    t = (m - 1.0f) / (m + 1.0f); t2 = t * t;
    s = t * (1.0f + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 * (1.0f / 9)))));           /* 0.5*ln(m) */
    return (float)e + 2.0f * s * 1.4426950408889634f;
}

typedef void *(*PFN_GetCVar)(void *console, const char *name);
typedef int (*PFN_GetIVal)(void *cvar);
static void *g_cv_width, *g_cv_native, *g_cv_fsnative; static int g_cv_tried;
static int cvar_int(void *cv) { return cv ? ((PFN_GetIVal)VT(cv, 2))(cv) : 0; }
static void cvars_lookup(void)
{
    void *console;
    if (g_cv_tried || !g_wh_base) return;
    g_cv_tried = 1;
    console = *(void **)(g_wh_base + RVA_CONSOLE);
    if (!console || !readable((BYTE *)console, 8) || !readable(*(BYTE **)console, 8 * 24)) { LOG(log_s("console object not ready: mip bias from the renderer sizes only")); g_cv_tried = 0; return; }
    g_cv_width = ((PFN_GetCVar)VT(console, 23))(console, "r_Width");
    g_cv_native = ((PFN_GetCVar)VT(console, 23))(console, "r_NativeWidth");
    g_cv_fsnative = ((PFN_GetCVar)VT(console, 23))(console, "r_FullscreenNativeRes");
    LOG(log_s("cvars: r_Width "); log_i(cvar_int(g_cv_width)); log_s(", r_NativeWidth "); log_i(cvar_int(g_cv_native)); log_s(", r_FullscreenNativeRes "); log_i(cvar_int(g_cv_fsnative)));
}
/* KCDDLSS.ini Exec = "cvar value; cvar value": applied once at frame 3 (after the game's profile has applied its own
 * values, which override user.cfg for e.g. r_ColorGrading). ICVar vtable (FINDINGS_KCD_ICVAR.md): 2 GetIVal, 7 Set(int),
 * 10 ForceSet(const char*) (bypasses cheat/read-only bits); IConsole slot 35 ExecuteString(cmd, silent, deferred) fallback. */
typedef void (*PFN_ForceSet)(void *cv, const char *v);
typedef void (*PFN_ExecStr)(void *console, const char *cmd, BYTE silent, BYTE deferred);
static void *console_ptr(void)
{
    void *console;
    if (!g_wh_base) return NULL;
    console = *(void **)(g_wh_base + RVA_CONSOLE);
    if (!console || !readable((BYTE *)console, 8) || !readable(*(BYTE **)console, 8 * 36)) return NULL;
    return console;
}
/* run "name value" / "?name" / "command args" items separated by ';' or newlines. Cvars: ForceSet + read back (immediate,
 * render thread); unknown names: IConsole::ExecuteString deferred (runs on the main thread in the next Update). */
/* "!name value": write the cvar's value field directly (CXConsoleVariableInt/Float: int/float at +0x48; the *Ref
 * variants keep a pointer to the external variable there), bypassing OnBeforeVarChange which rejects guarded cvars
 * (r_DepthOfField, r_PostProcessGameFx, r_PostProcessFilters, r_HDRVignetting...). Change callbacks do not run. */
typedef float (*PFN_GetFVal)(void *cv);
static int cvar_force_write(void *cv, const char *val, int *before, int *after)
{
    int type = ((PFN_GetIVal)VT(cv, 14))(cv); BYTE *field = (BYTE *)cv + 0x48; ULONG_PTR q; BYTE *target = NULL;
    if (!readable(field, 8)) return 0;
    q = *(ULONG_PTR *)field;
    if (type == 1) {
        int v = cvar_int(cv); *before = v;
        if (*(int *)field == v) target = field;
        else if (q > 0x10000 && readable((BYTE *)q, 4) && *(int *)q == v) target = (BYTE *)q;
        if (!target) return 0;
        *(int *)target = (int)parse_num(val); *after = cvar_int(cv); return 1;
    }
    if (type == 2) {
        float v = ((PFN_GetFVal)VT(cv, 4))(cv); *before = (int)(v * 1000);
        if (*(float *)field == v) target = field;
        else if (q > 0x10000 && readable((BYTE *)q, 4) && *(float *)q == v) target = (BYTE *)q;
        if (!target) return 0;
        *(float *)target = parse_num(val); *after = (int)(((PFN_GetFVal)VT(cv, 4))(cv) * 1000); return 1;
    }
    return 0;
}
static void exec_commands(char *p, const char *src)
{
    void *console = console_ptr(); char name[64], val[128]; int i;
    if (!console) { LOG(log_s("exec("); log_s(src); log_s("): console not ready, skipped")); return; }
    while (*p) {
        void *cv; char cmd[200]; int query = 0;
        while (*p == ' ' || *p == ';' || *p == '\r' || *p == '\n' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (*p == '?') { query = 1; p++; }
        else if (*p == '!') { query = 2; p++; }
        i = 0; while (*p && *p != ' ' && *p != ';' && *p != '=' && *p != '\r' && *p != '\n' && i < 63) name[i++] = *p++; name[i] = 0;
        while (*p == ' ' || *p == '=') p++;
        i = 0; while (*p && *p != ';' && *p != '\r' && *p != '\n' && i < 127) val[i++] = *p++; val[i] = 0;
        while (i > 0 && val[i - 1] == ' ') val[--i] = 0;
        if (streq(name, "kick")) { kick_activate("kick command"); continue; }   /* 0.8.0: experiment: the renderer re-activate routine (minimizes the window too) */
        if (streq(name, "fxoff") || streq(name, "fxon") || streq(name, "fxwrap")) { fx_set(val, name[2] == 'o' ? 1 : name[2] == 'w' ? 2 : 0, src); continue; }
        if (streq(name, "mode")) { int m = mode_parse(val); if (m == -2) LOG(log_s("exec("); log_s(src); log_s("): mode "); log_s(val); log_s(": unknown (Native, UltraQuality, Quality, Balanced, Performance, UltraPerformance, Auto)")); else mode_apply(m, src); continue; }
        if (streq(name, "preset")) {
            int pv = 0, pk = preset_parse(val, &pv);
            if (pk < 0) LOG(log_s("exec("); log_s(src); log_s("): preset "); log_s(val); log_s(": unknown (Transformer, CNN, Default, or a letter E..O)"));
            else { g_cfg.preset = pk; g_cfg.preset_val = pv; dl_release_feature(); black_ms(g_cfg.blackms / 2); LOGC(log_s("exec("); log_s(src); log_s("): preset "); log_s(val); log_s(": DLSS feature re-created next frame")); if (!streq(src, "menu")) dlsspreset_sync(); }
            continue;
        }
        if (streq(name, "mipbias")) {
            if (ci_eq(val, "off")) g_cfg.mipbias = 0; else if (ci_eq(val, "on")) g_cfg.mipbias = 1; else { g_cfg.mipbias = 1; g_cfg.mipoff = parse_num(val); }
            LOG(log_s("exec("); log_s(src); log_s("): mip bias "); if (g_cfg.mipbias) { log_s("on, offset "); log_i((int)(g_cfg.mipoff * 1000)); log_s(" milli (applied from the next frame)"); } else log_s("off"));
            continue;
        }
        if (streq(name, "blink")) { g_blink_end = GetTickCount() + (DWORD)(parse_num(val) * 1000.0f); LOG(log_s("exec("); log_s(src); log_s("): blink test: 2 s black / 2 s normal for "); log_s(val); log_s(" s")); continue; }
        if (streq(name, "black")) { black_ms((int)parse_num(val)); LOG(log_s("exec("); log_s(src); log_s("): black-out for "); log_i((int)parse_num(val)); log_s(" ms")); continue; }
        if (streq(name, "dump")) { g_dump_req = 1; LOG(log_s("exec("); log_s(src); log_s("): dump of the DLSS input and output requested (KCDDLSS_in.ppm / KCDDLSS_out.ppm)")); continue; }
        cv = ((PFN_GetCVar)VT(console, 23))(console, name);
        if (query == 2) {
            int before = 0, after = 0;
            if (cv && cvar_force_write(cv, val, &before, &after)) { LOG(log_s("exec("); log_s(src); log_s("): forced "); log_s(name); log_s(" "); log_s(val); log_s(": was "); log_i(before); log_s(", now "); log_i(after)); }
            else LOG(log_s("exec("); log_s(src); log_s("): cannot force "); log_s(name); log_s(cv ? " (field not recognised)" : " (unknown cvar)"));
        } else if (query || (cv && !val[0])) {
            LOG(log_s("exec("); log_s(src); log_s("): "); log_s(name); log_s(" = "); if (cv) { if (((PFN_GetIVal)VT(cv, 14))(cv) == 2) log_i((int)(((float (*)(void *))VT(cv, 4))(cv) * 1000)), log_s(" (float x1000)"); else log_i(cvar_int(cv)); } else log_s("unknown cvar"));
        } else if (cv) {
            int before = cvar_int(cv), after;
            ((PFN_ForceSet)VT(cv, 10))(cv, val);
            after = cvar_int(cv);
            if (after == before) {   /* did not stick (guard flags): try the console command path */
                i = 0; while (name[i] && i < 63) { cmd[i] = name[i]; i++; } cmd[i++] = ' '; { int j = 0; while (val[j] && j < 127) cmd[i++] = val[j++]; } cmd[i] = 0;
                ((PFN_ExecStr)VT(console, 35))(console, cmd, 1, 0);
                after = cvar_int(cv);
            }
            LOG(log_s("exec("); log_s(src); log_s("): "); log_s(name); log_s(" "); log_s(val); log_s(": was "); log_i(before); log_s(", now "); log_i(after); log_s(after == before ? " (unchanged)" : ""));
        } else {
            i = 0; while (name[i] && i < 63) { cmd[i] = name[i]; i++; } if (val[0]) { cmd[i++] = ' '; { int j = 0; while (val[j] && j < 127) cmd[i++] = val[j++]; } } cmd[i] = 0;
            ((PFN_ExecStr)VT(console, 35))(console, cmd, 0, 1);
            LOG(log_s("exec("); log_s(src); log_s("): command queued: "); log_s(cmd));
        }
    }
}
/* ---- 0.6.0 DLSS modes: the render resolution follows the NVIDIA factor of the mode (NGX optimal settings when the core is up, else
 * the documented factors 1.5/1.72/2.0/3.0), applied through r_Width/r_Height; the game resizes its targets, the stretch hook sees the
 * new render size and re-creates the DLSS feature with the mode's quality value. Native = the game renders at native size (no stretch). */
static void mode_apply(int mode, const char *src)
{
    UINT nw = 0, nh = 0, rw = 0, rh = 0; char cmd[96], num[12]; const char *how = "factor table"; int i = 0, j;
    static const float factor[6] = { 2.0f, 1.72f, 1.5f, 3.0f, 1.0f / 0.83f, 1.0f };   /* UltraQuality (0.7.4, user): 83 % render scale, DLSS AA with the least upscaling */
    if (mode < -1 || mode > 5) { LOG(log_s("exec("); log_s(src); log_s("): mode: use Native, UltraQuality, Quality, Balanced, Performance, UltraPerformance or Auto")); return; }
    g_cfg.mode = mode;
    if (mode == -1) { g_mode_rw = g_mode_rh = 0; LOG(log_s("exec("); log_s(src); log_s("): mode Auto: the game's resolution decides the DLSS quality")); return; }
    if (g_true_nw >= 640 && g_true_nh >= 480) { nw = g_true_nw; nh = g_true_nh; }
    else {   /* 0.7.5: before the first upscaled frame the renderer's native fields still hold the pre-flip output = the profile's res_x/res_y,
              * which the game saves as OUR render size at exit (run073: 3840x2160 -> 1920x1080): a player whose stretch never worked would get a
              * smaller "native" every session. In fullscreen with r_FullscreenNativeRes the output is the desktop mode: read it from the
              * registry (ENUM_REGISTRY_SETTINGS = the desktop, not a temporary exclusive mode). */
        void *cvf = cvar_find("r_Fullscreen"), *cvn = cvar_find("r_FullscreenNativeRes"); DEVMODEW dm;
        memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        if (cvf && cvar_int(cvf) && cvn && cvar_int(cvn) && EnumDisplaySettingsW(NULL, ENUM_REGISTRY_SETTINGS, &dm) && dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) { nw = dm.dmPelsWidth; nh = dm.dmPelsHeight; how = "factor table, desktop size"; }
        else if (g_wh_base) { BYTE *r = g_wh_base + RVA_RENDERER; nw = *(UINT *)(r + R_NATIVE_W); nh = *(UINT *)(r + R_NATIVE_H); }
    }
    if (nw < 640 || nh < 480 || nw > 16384 || nh > 16384) { nw = (UINT)GetSystemMetrics(SM_CXSCREEN); nh = (UINT)GetSystemMetrics(SM_CYSCREEN); }
    if (mode == 5) { rw = nw; rh = nh; how = "native"; }
    else if (g_dl.disabled || g_no_upscaler) { rw = nw; rh = nh; how = "no upscaler available: native"; }
    else {
        if (mode != 4 && g_ngx.inited && g_ngx.available) { ngx_log_optimal(nw, nh, mode, MODE_NAMES[mode], &rw, &rh); if (rw && rh) how = "NGX optimal settings"; }   /* UltraQuality: NGX has no size for it, the table's 0.83 is the point */
        if (!rw || !rh) { rw = (UINT)((float)nw / factor[mode] + 0.5f); rh = (UINT)((float)nh / factor[mode] + 0.5f); }
    }
    if (g_dl.feature && g_dl.quality != mode) dl_release_feature();   /* same render size, other quality value: re-create */
    if (g_dl.feature && (g_dl.rw != rw || g_dl.rh != rh || g_dl.quality != mode)) black_ms(g_cfg.blackms);   /* hide the resize (QoL, user 2026-11-08) */
    for (j = 0; "r_Width "[j]; j++) cmd[i++] = "r_Width "[j];
    utoa_w(num, rw); for (j = 0; num[j]; j++) cmd[i++] = num[j];
    for (j = 0; "; r_Height "[j]; j++) cmd[i++] = "; r_Height "[j];
    utoa_w(num, rh); for (j = 0; num[j]; j++) cmd[i++] = num[j];
    cmd[i] = 0;
    g_mode_rw = rw; g_mode_rh = rh; g_mode_nw = nw; g_mode_nh = nh;
    LOGC(log_s("exec("); log_s(src); log_s("): mode "); log_s(MODE_NAMES[mode]); log_s(": native "); log_u(nw); log_s("x"); log_u(nh); log_s(" -> render "); log_u(rw); log_s("x"); log_u(rh); log_s(" ("); log_s(how); log_s(")"));
    exec_commands(cmd, src);
    if (!streq(src, "menu")) dlssmode_sync(mode);
}
/* 0.7.0: options-menu cvar. The menu mod (tools/menu-mod, Mods/KCDDLSS) binds an "DLSS / Upscaling" row in Advanced Graphics to the
 * console variable r_DLSSMode through the UI flowgraph's Debug:ConsoleVariable node (FINDINGS_KCD_UI_PAKS.md). Menu values:
 * 0 Off (native), 1 Ultra Quality (0.7.4), 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance. The plugin registers the cvar itself (IConsole::RegisterInt,
 * slot from FINDINGS_KCD_CONSOLE.md), polls it every tick, applies the mode and writes "Mode = X" back into KCDDLSS.ini (the game does
 * not persist foreign cvars in attributes.xml). "mode X" console/ini changes update the cvar so the menu row shows the truth. */
#define CON_SLOT_REGISTERINT 3          /* IConsole vtable slot 3 = RegisterInt(this, name, int value, int flags, help, cb) (FINDINGS_KCD_CONSOLE.md); -1 = feature off */
#define CVAR_DLSSMODE "r_DLSSMode"
#define CVAR_DLSSPRESET "r_DLSSPreset"   /* 0.7.2: menu row "DLSS Model": 0 Transformer, 1 CNN (ini Preset =) */
static void *g_cv_dlsspreset; static int g_dlsspreset_last = -99;
typedef void *(*PFN_RegisterInt)(void *console, const char *name, int value, int flags, const char *help, void *callback);
static void *g_cv_dlssmode; static int g_dlssmode_last = -99;
static const int MENU2MODE[6] = { 5, 4, 2, 1, 0, 3 };
#define MENU_MODE_UNSUPPORTED 6     /* 0.7.4: 7th choice of the row, pinned when no upscaler exists (g_no_upscaler) */
#define MENU_PRESET_UNSUPPORTED 2   /* 3rd choice of the DLSS Model row */
static int mode2menu(int m) { switch (m) { case 0: return 4; case 1: return 3; case 2: return 2; case 3: return 5; case 4: return 1; case 5: return 0; default: return 4; } }
/* rewrite the "Mode = ..." line of KCDDLSS.ini (first line whose key is Mode; appended if missing); keeps the rest byte for byte */
static int key_at(const char *buf, int k, const char *key)   /* "Key" at buf[k], case-insensitive, followed by space, tab or '=' */
{
    int j; for (j = 0; key[j]; j++) { char c = buf[k + j]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != key[j]) return 0; }
    return buf[k + j] == ' ' || buf[k + j] == '=' || buf[k + j] == '\t';
}
static void ini_save_key(const char *key, const char *lkey, const char *name)   /* rewrite "Key = name" in KCDDLSS.ini (lkey = key in lower case) */
{
    static char buf[8192], out[8192 + 64]; WCHAR path[MAX_PATH + 40]; HANDLE h; DWORD n = 0, w; int i = 0, o = 0, done = 0;
    path[0] = 0; wcat(path, MAX_PATH + 40, g_dir); wcat(path, MAX_PATH + 40, L"\\KCDDLSS.ini");
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL)) n = 0; CloseHandle(h); }
    buf[n] = 0;
    while (i < (int)n) {
        int ls = i, k = i, e; while (buf[k] == ' ' || buf[k] == '\t') k++;
        e = i; while (e < (int)n && buf[e] != '\n') e++;                      /* e = end of line (at '\n' or n) */
        if (!done && key_at(buf, k, lkey)) {
            int j; const char *t = key; while (*t) out[o++] = *t++; out[o++] = ' '; out[o++] = '='; out[o++] = ' '; for (j = 0; name[j]; j++) out[o++] = name[j];
            if (e > ls && buf[e - 1] == '\r') out[o++] = '\r';
            done = 1;
        } else { int j; for (j = ls; j < e; j++) out[o++] = buf[j]; }
        if (e < (int)n) out[o++] = '\n';
        i = e + 1;
    }
    if (!done) { int j; const char *t = key; if (o && out[o - 1] != '\n') { out[o++] = '\r'; out[o++] = '\n'; } while (*t) out[o++] = *t++; out[o++] = ' '; out[o++] = '='; out[o++] = ' '; for (j = 0; name[j]; j++) out[o++] = name[j]; out[o++] = '\r'; out[o++] = '\n'; }
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { LOG(log_s("menu: KCDDLSS.ini not writable, "); log_s(key); log_s(" not saved")); return; }
    WriteFile(h, out, (DWORD)o, &w, NULL); CloseHandle(h);
    LOG(log_s("menu: KCDDLSS.ini "); log_s(key); log_s(" = "); log_s(name); log_s(" saved"));
}
static void ini_save_mode(int mode)
{
    const char *name = (mode >= 0 && mode <= 5) ? MODE_NAMES[mode] : "Auto";
    ini_save_key("Mode", "mode", name);
}
static void dlssmode_register(void)
{
    void *console; static int tried;
    if (g_cv_dlssmode || tried || CON_SLOT_REGISTERINT < 0) return;
    console = console_ptr(); if (!console) return;
    tried = 1;
    g_cv_dlssmode = cvar_find(CVAR_DLSSMODE);
    if (!g_cv_dlssmode)
        g_cv_dlssmode = ((PFN_RegisterInt)VT(console, CON_SLOT_REGISTERINT))(console, CVAR_DLSSMODE, g_no_upscaler ? MENU_MODE_UNSUPPORTED : mode2menu(g_cfg.mode), 0,
            "KCD DLSS plugin: upscaling mode set from the options menu. 0 Off (native), 1 Ultra Quality, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance, 6 Unsupported (no upscaler on this system)", NULL);
    if (g_cv_dlssmode) { g_dlssmode_last = cvar_int(g_cv_dlssmode); LOG(log_s("menu: cvar " CVAR_DLSSMODE " registered, value "); log_i(g_dlssmode_last)); }
    else LOG(log_s("menu: cvar " CVAR_DLSSMODE " could not be registered (RegisterInt returned NULL)"));
    g_cv_dlsspreset = cvar_find(CVAR_DLSSPRESET);
    if (!g_cv_dlsspreset)
        g_cv_dlsspreset = ((PFN_RegisterInt)VT(console, CON_SLOT_REGISTERINT))(console, CVAR_DLSSPRESET, g_no_upscaler ? MENU_PRESET_UNSUPPORTED : (g_cfg.preset == 1 ? 1 : 0), 0,
            "KCD DLSS plugin: DLSS model set from the options menu. 0 Transformer (DLSS 4), 1 CNN (DLSS 3), 2 Unsupported (no upscaler on this system)", NULL);
    if (g_cv_dlsspreset) { g_dlsspreset_last = cvar_int(g_cv_dlsspreset); LOG(log_s("menu: cvar " CVAR_DLSSPRESET " registered, value "); log_i(g_dlsspreset_last)); }
    else LOG(log_s("menu: cvar " CVAR_DLSSPRESET " could not be registered"));
}
static void dlsspreset_sync(void)         /* after a preset change from the ini/console: show it in the menu cvar */
{
    int v = g_no_upscaler ? MENU_PRESET_UNSUPPORTED : (g_cfg.preset == 1 ? 1 : 0);
    if (!g_cv_dlsspreset || cvar_int(g_cv_dlsspreset) == v) { g_dlsspreset_last = v; return; }
    ((PFN_ForceSet)VT(g_cv_dlsspreset, 10))(g_cv_dlsspreset, v == 2 ? "2" : v ? "1" : "0"); g_dlsspreset_last = cvar_int(g_cv_dlsspreset);
}
static void dlssmode_sync(int mode)        /* after a mode change from the ini/console: show it in the menu cvar */
{
    char num[12]; int v = g_no_upscaler ? MENU_MODE_UNSUPPORTED : mode2menu(mode);
    if (!g_cv_dlssmode || cvar_int(g_cv_dlssmode) == v) { g_dlssmode_last = v; return; }
    utoa_w(num, (unsigned)v); ((PFN_ForceSet)VT(g_cv_dlssmode, 10))(g_cv_dlssmode, num); g_dlssmode_last = cvar_int(g_cv_dlssmode);
}
static void dlssmode_poll(void)
{
    int v;
    if (!g_cv_dlssmode) return;
    v = cvar_int(g_cv_dlssmode);
    if (v == g_dlssmode_last) return;
    g_dlssmode_last = v;
    if (g_no_upscaler || v == MENU_MODE_UNSUPPORTED) {   /* 0.7.4: no upscaler -> the row stays on "Unsupported"; with one, "Unsupported" is not a choice: snap back */
        LOG(log_s("menu: " CVAR_DLSSMODE " -> "); log_i(v); log_s(g_no_upscaler ? ": no upscaler on this system, row pinned to Unsupported" : ": Unsupported is not selectable here, back to the current mode"));
        dlssmode_sync(g_cfg.mode); return;
    }
    if (v < 0 || v > 5) { LOG(log_s("menu: " CVAR_DLSSMODE " "); log_i(v); log_s(" out of range (0..5), ignored")); return; }
    LOGC(log_s("menu: " CVAR_DLSSMODE " -> "); log_i(v); log_s(" ("); log_s(MENU2MODE[v] == 5 ? "Native" : MODE_NAMES[MENU2MODE[v]]); log_s(")"));
    if (MENU2MODE[v] != g_cfg.mode) { mode_apply(MENU2MODE[v], "menu"); ini_save_mode(MENU2MODE[v]); }
}
static void dlsspreset_poll(void)
{
    int v; char cmd[24]; const char *name; int j;
    if (!g_cv_dlsspreset) return;
    v = cvar_int(g_cv_dlsspreset);
    if (v == g_dlsspreset_last) return;
    g_dlsspreset_last = v;
    if (g_no_upscaler || v == MENU_PRESET_UNSUPPORTED) { LOG(log_s("menu: " CVAR_DLSSPRESET " -> "); log_i(v); log_s(g_no_upscaler ? ": pinned to Unsupported" : ": Unsupported is not selectable here, back to the current model")); dlsspreset_sync(); return; }
    if (v < 0 || v > 1) { LOG(log_s("menu: " CVAR_DLSSPRESET " "); log_i(v); log_s(" out of range (0..1), ignored")); return; }
    name = v ? "CNN" : "Transformer";
    LOGC(log_s("menu: " CVAR_DLSSPRESET " -> "); log_i(v); log_s(" ("); log_s(name); log_s(")"));
    if ((g_cfg.preset == 1) == (v == 1)) return;
    { const char *t = "preset "; j = 0; while (*t) cmd[j++] = *t++; t = name; while (*t) cmd[j++] = *t++; cmd[j] = 0; }
    exec_commands(cmd, "menu");
    ini_save_key("Preset", "preset", name);
}
/* 0.6.5: what DLSS needs from the game, forced by the plugin (user 2026-11-08: "force it live with the plugin init"), so a stock
 * install works without user.cfg edits: scene at r_Width x r_Height stretched to the native back buffer, the game's TAA off (DLSS
 * replaces it; its jitter patch applies in AA mode 0), per-object motion vectors in the velocity buffer, 100 m object-MV range.
 * GIFullRes: SVOGI cone tracing at the render size instead of half of it (e_svoTI_ResScaleBase, help text "2=half res"); at
 * DLSS Performance half res would be a quarter of native, upscaled twice. The game's profile (attributes.xml) re-applies its
 * own r_AntialiasingMode / resolution after a settings-menu change: the watchdog below re-forces within ~3 s. */
static void *cvar_find(const char *name) { void *console = console_ptr(); return console ? ((PFN_GetCVar)VT(console, 23))(console, name) : NULL; }
/* 0.7.5 (BUG-1): a LIVE r_AntialiasingMode change while the game is still starting up (profile value 2/3 -> our 0 at the 6 s tick) left
 * the native-res stretch drawing nothing for seconds: black screen (run072: still nothing 2.5 s after the minimize/restore kick, then it
 * recovered by itself; the same switch at the main menu is harmless; runs whose profile already had 0 never failed) = the PostAA.cfx
 * permutation is recompiled asynchronously and the UpscaleImage draw is skipped until it is ready. So the AA mode is forced to 0 as soon
 * as the cvar exists (device creation, before the first frame) and PINNED: the int-cvar class's Set(int) (ICVar slot 7, where Set(float),
 * Set(const char*) and ForceSet all end up, FINDINGS_KCD_ICVAR.md) is hooked and every value for this one cvar becomes 0. */
typedef void (*PFN_SetInt)(void *cv, int v); typedef void (*PFN_SetFloat)(void *cv, float v); typedef void (*PFN_SetStr)(void *cv, const char *v);
static void *g_cv_aa; static void **g_aa_vt; static PFN_SetInt g_aa_orig_set; static PFN_SetFloat g_aa_orig_setf; static PFN_SetStr g_aa_orig_sets, g_aa_orig_force; static int g_aa_pin_logs;
/* All four entry points are pinned (run073): the profile's Set(const char*) reaches Set(int) by a DIRECT call, the cvar's OnChange callback
 * (0x181fbe3dc) then calls Set(int) through the vtable (our hook -> 0) but afterwards stores its own stale value into the renderer's
 * effective AA copy (0x18358fcc4) -> the cvar read 0 while the renderer ran SMAA 1TX. With the string/float/force entries pinned too the
 * value 2 never exists. */
static int aa_pinned(void *cv, int v, const char *how)
{
    if (cv != g_cv_aa || !g_cfg.prereq || v == 0) return 0;
    if (g_aa_pin_logs++ < 5) LOG(log_s("pin: r_AntialiasingMode "); log_i(v); log_s(" requested via "); log_s(how); log_s(" (profile load / settings menu): kept at 0, DLSS replaces the AA"));
    return 1;
}
static int str_to_int(const char *v) { int n = 0, neg = 0; if (!v) return 0; while (*v == ' ') v++; if (*v == '-') { neg = 1; v++; } while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0'); return neg ? -n : n; }
static void my_cvar_set_int(void *cv, int v) { if (aa_pinned(cv, v, "Set(int)")) v = 0; g_aa_orig_set(cv, v); }
static void my_cvar_set_float(void *cv, float v) { if (aa_pinned(cv, (int)v, "Set(float)")) { g_aa_orig_set(cv, 0); return; } g_aa_orig_setf(cv, v); }
static void my_cvar_set_str(void *cv, const char *v) { if (aa_pinned(cv, str_to_int(v), "Set(string)")) { g_aa_orig_set(cv, 0); return; } g_aa_orig_sets(cv, v); }
static void my_cvar_force(void *cv, const char *v) { if (aa_pinned(cv, str_to_int(v), "ForceSet")) { g_aa_orig_force(cv, "0"); return; } g_aa_orig_force(cv, v); }
static void aa_pin_install(const char *src)
{
    void **vt; DWORD old, tmp; int was;
    if (g_aa_vt || !g_cfg.prereq || !g_wh_base) return;
    g_cv_aa = cvar_find("r_AntialiasingMode");
    if (!g_cv_aa) return;   /* not registered yet: retried at the next device / tick */
    vt = *(void ***)g_cv_aa;
    if (!readable((BYTE *)vt, 11 * sizeof(void *)) || !vt[7] || !vt[8] || !vt[9] || !vt[10] || !VirtualProtect(&vt[7], 4 * sizeof(void *), PAGE_READWRITE, &old)) {
        g_aa_vt = (void **)1; LOG(log_s("pin: r_AntialiasingMode vtable not writable: no pin (the 6 s tick forces it as before)")); return;
    }
    g_aa_orig_set = (PFN_SetInt)vt[7]; g_aa_orig_setf = (PFN_SetFloat)vt[8]; g_aa_orig_sets = (PFN_SetStr)vt[9]; g_aa_orig_force = (PFN_SetStr)vt[10];
    vt[7] = (void *)my_cvar_set_int; vt[8] = (void *)my_cvar_set_float; vt[9] = (void *)my_cvar_set_str; vt[10] = (void *)my_cvar_force;
    VirtualProtect(&vt[7], 4 * sizeof(void *), old, &tmp);
    g_aa_vt = vt;
    was = cvar_int(g_cv_aa);
    if (was != 0) ((PFN_ForceSet)VT(g_cv_aa, 10))(g_cv_aa, "0");
    LOG(log_s("pin("); log_s(src); log_s("): r_AntialiasingMode was "); log_i(was); log_s(", now "); log_i(cvar_int(g_cv_aa));
        log_s("; Set/ForceSet of its cvar class hooked: the value stays 0 (no live AA switch at startup or on a settings Apply); renderer AA copy "); log_i(*(int *)(g_wh_base + RVA_AA_MODE)));
}
#define PREREQ_CMDS "r_FullscreenNativeRes 1; r_AntialiasingMode 0; r_MotionVectors 1; r_MotionBlurGBufferVelocity 1"   /* 0.6.8: MaxViewDist left at the game's 32 m (user) */
#define GI_CMDS "e_svoTI_ResScaleBase 1"
/* 0.6.8: switching r_FullscreenNativeRes 0 -> 1 live in exclusive fullscreen left the screen black until the user alt-tabbed (the
 * focus loss/regain resets the swap chain; run060). If the stretch keeps failing after that switch, the plugin does the same:
 * minimize + restore its own window once. */
static int g_flipped, g_kicked; static DWORD g_flip_time, g_kick_time; static HWND g_wnd;
/* 0.7.5 (BUG-1): the restore half of the kick runs on its own thread: the frame tick stops with the render thread if the game pauses
 * while minimized (a 1080p/RTX 3080 log ended at the kick line). 2.5 s later the window state is logged and the stretch errors are
 * re-armed, so a kick that did not help shows up in the log. */
static DWORD WINAPI kick_thread(LPVOID p)
{
    (void)p;
    Sleep(400);
    PostMessageW(g_wnd, WM_SYSCOMMAND, SC_RESTORE, 0); g_kicked = 2;
    Sleep(2500);
    g_dl.errs = 0;
    LOG(log_s("display: 2.5 s after the kick: window "); log_s(IsIconic(g_wnd) ? "still minimized" : "restored"); log_s(", ");
        log_s(GetForegroundWindow() == g_wnd ? "foreground" : "NOT foreground"); log_s(", upscaled frames "); log_u(g_dl.frames);
        log_s(", stretch errors re-armed (the next failures are logged)"));
    return 0;
}
static BOOL CALLBACK find_wnd_cb(HWND h, LPARAM lp) { DWORD pid = 0; GetWindowThreadProcessId(h, &pid); if (pid == (DWORD)lp && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { g_wnd = h; return FALSE; } return TRUE; }
static void prereq_apply(const char *src)
{
    char buf[256]; int i; void *cv;
    if (g_cfg.prereq && !g_startup_done) { cv = cvar_find("r_FullscreenNativeRes"); if (cv && cvar_int(cv) == 0) g_flipped = 1; }
    if (g_cfg.prereq) { for (i = 0; PREREQ_CMDS[i] && i < 255; i++) buf[i] = PREREQ_CMDS[i]; buf[i] = 0; exec_commands(buf, src); }
    if (g_cfg.gifull) { for (i = 0; GI_CMDS[i] && i < 255; i++) buf[i] = GI_CMDS[i]; buf[i] = 0; exec_commands(buf, src); }
}
static void cvars_exec_once(void)
{
    if (g_startup_done) return;
    if (!console_ptr()) return;   /* retry next frame */
    prereq_apply("init");
    LOG(log_s("exec(init): renderer AA mode copy "); log_i(*(int *)(g_wh_base + RVA_AA_MODE)); log_s(" (0 = the game's SMAA/TAA is off, DLSS does the AA)"));
    g_startup_done = 1;
    if (g_cfg.exec[0]) exec_commands(g_cfg.exec, "ini");
    if (g_cfg.mode != -1) mode_apply(g_cfg.mode, "ini");
    g_flip_time = GetTickCount();
}
static void watchdog(void)
{
    static DWORD last_fix; static int fixes; DWORD now = GetTickCount(); void *cv; int bad = 0, badmode = 0;
    if (!g_startup_done || (int)(now - last_fix) < 3000) return;   /* 0.7.1: no fix limit (every menu Apply puts the values back), logs thinned */
    if (g_cfg.prereq) {
        cv = cvar_find("r_AntialiasingMode"); if (cv && cvar_int(cv) != 0) bad = 1;
        cv = cvar_find("r_FullscreenNativeRes"); if (cv && cvar_int(cv) != 1) bad = 1;
    }
    if (g_cfg.gifull) { cv = cvar_find("e_svoTI_ResScaleBase"); if (cv && cvar_int(cv) != 1) bad = 1; }
    if (!bad && !badmode) return;
    last_fix = now; fixes++;
    if (fixes <= 8 || (fixes % 20) == 0) LOG(log_s("watchdog: the game changed a prerequisite cvar (settings menu / profile re-applied): re-forcing (fix "); log_i(fixes); log_s(")"));
    if (bad) prereq_apply("watchdog");
}
/* 0.7.1 fast watchdog: every 64th render-target bind, no delay. The settings menu's Apply puts the profile's values back
 * (r_FullscreenNativeRes 0) and takes r_Width/r_Height, which hold OUR render size, as the resolution, so the renderer would switch
 * the output to 1280x720 at the next frame start (run 064: 4K DLSS feature, "Resolution: 1280x720, Native: 1280x720",
 * DXGI_ERROR_DEVICE_REMOVED). Answer within the same frame. A r_Width that is neither our render size nor the known output size is
 * a real resolution change: adopt it as the new output size. Not for Auto (the game owns the resolution) or Native (no stretch). */
static void watchdog_fast(void)
{
    static void *cv_fs, *cv_w, *cv_h, *cv_nw, *cv_nh; static int looked, win_n, logged; static DWORD win_t0, hold_until;
    DWORD now = GetTickCount(); UINT w, h, ow, oh; int fs, kind; char cmd[80], num[12]; int i = 0, j;
    if (!g_startup_done || g_cfg.mode < 0 || g_cfg.mode == 5 || !g_mode_rw) return;
    if (!looked) { looked = 1; cv_fs = cvar_find("r_FullscreenNativeRes"); cv_w = cvar_find("r_Width"); cv_h = cvar_find("r_Height"); cv_nw = cvar_find("r_NativeWidth"); cv_nh = cvar_find("r_NativeHeight"); }
    if (!cv_w || !cv_h) return;
    if (hold_until && (int)(hold_until - now) > 0) return;
    w = (UINT)cvar_int(cv_w); h = (UINT)cvar_int(cv_h); fs = cv_fs ? cvar_int(cv_fs) : 1;
    ow = cv_nw ? (UINT)cvar_int(cv_nw) : 0; oh = cv_nh ? (UINT)cvar_int(cv_nh) : 0;
    if (w == g_mode_rw && h == g_mode_rh) {
        if (g_cfg.prereq && fs != 1) kind = 1;                                                  /* r_FullscreenNativeRes put back */
        else if (g_true_nw && ow && (ow != g_true_nw || oh != g_true_nh)) kind = 2;              /* output size followed r_Width */
        else return;
    } else if (g_true_nw && (w != g_true_nw || h != g_true_nh) && w >= 640 && h >= 480 && w <= 16384 && h <= 16384) {
        kind = 3; g_true_nw = w; g_true_nh = h;                                                   /* a real resolution change */
    } else kind = 4;                                                                             /* r_Width = output size again */
    if ((int)(now - win_t0) > 10000) { win_t0 = now; win_n = 0; }
    if (++win_n > 20) { hold_until = now + 10000; LOG(log_s("watchdog: 20 fixes in 10 s, something fights back: pausing for 10 s")); return; }
    if (logged < 10 || (win_n % 10) == 0) {
        logged++;
        LOG(log_s("watchdog: "); log_s(kind == 1 ? "the game put r_FullscreenNativeRes back" : kind == 2 ? "the output size followed our render size" : kind == 3 ? "the game changed the resolution: new output size" : "r_Width changed to the output size");
            log_s(" (r_Width "); log_u(w); log_s("x"); log_u(h); log_s(", r_NativeWidth "); log_u(ow); log_s("x"); log_u(oh); log_s(", native-res "); log_i(fs); log_s("): re-forcing now"));
    }
    if (g_cfg.prereq && fs != 1) prereq_apply("watchdog");
    if (g_true_nw && cv_nw && cv_nh && ow && oh && (ow != g_true_nw || oh != g_true_nh)) {   /* the game keeps r_NativeWidth at 0 (run 065): leave 0 alone */
        for (j = 0; "r_NativeWidth "[j]; j++) cmd[i++] = "r_NativeWidth "[j];
        utoa_w(num, g_true_nw); for (j = 0; num[j]; j++) cmd[i++] = num[j];
        for (j = 0; "; r_NativeHeight "[j]; j++) cmd[i++] = "; r_NativeHeight "[j];
        utoa_w(num, g_true_nh); for (j = 0; num[j]; j++) cmd[i++] = num[j];
        cmd[i] = 0;
        exec_commands(cmd, "watchdog");
    }
    if (kind >= 3) mode_apply(g_cfg.mode, "watchdog");
}
/* live console: Bin\Win64\KCDDLSS.cmd is polled every 30 frames; its lines are executed and the file deleted */
static void cmdfile_poll(void)
{
    static WCHAR path[MAX_PATH + 40]; HANDLE h; char buf[4096]; DWORD n = 0;
    if (!path[0]) { wcat(path, MAX_PATH + 40, g_dir); wcat(path, MAX_PATH + 40, L"\\KCDDLSS.cmd"); }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;   /* still being written: next poll */
    if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL)) n = 0;
    CloseHandle(h);
    if (!DeleteFileW(path)) { LOG(log_s("exec(cmd): cannot delete KCDDLSS.cmd, not executed")); return; }
    buf[n] = 0;
    exec_commands(buf, "cmd");
}
/* 0.6.5 frame tick: called from the OMSetRenderTargets hook (render thread, hundreds of times a frame, so every 64th call and at
 * most twice a second). Startup settings 6 s after the first bind if the stretch path has not run them (native boot: no stretch,
 * no dlss_frame), then the command file and the watchdog. */
static void frame_tick(void)
{
    static unsigned calls; static DWORD t0, last; DWORD now;
    if ((++calls & 63) != 0) return;
    now = GetTickCount();
    if (!t0) { t0 = now; return; }
    if (g_wh_base) watchdog_fast();
    if ((int)(now - last) < 500) return;
    last = now;
    if (!g_wh_base) return;
    if (!g_aa_vt) aa_pin_install("tick");
    if (!g_startup_done && (int)(now - t0) >= 6000) cvars_exec_once();
    if (g_startup_done && g_flipped && !g_kicked && g_dl.errs >= 3 && g_dl.frames == 0 && (int)(now - g_flip_time) > 1500) {
        g_wnd = NULL; EnumWindows(find_wnd_cb, (LPARAM)GetCurrentProcessId());
        if (g_wnd) {
            HANDLE th; g_kicked = 1; g_kick_time = now;
            PostMessageW(g_wnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            LOG(log_s("display: stretch failing after the native-res switch: minimize/restore kick (swap chain reset)"));
            th = CreateThread(NULL, 0, kick_thread, NULL, 0, NULL);
            if (th) CloseHandle(th); else { Sleep(400); PostMessageW(g_wnd, WM_SYSCOMMAND, SC_RESTORE, 0); g_kicked = 2; }
        } else { g_kicked = 2; LOG(log_s("display: stretch failing after the native-res switch, but no window found for the kick")); }
    }
    if (g_startup_done) {
        if (g_fx_pending && fx_scan()) { g_fx_pending = 0; LOG(log_s("exec(ini): post effects ready now, Exec line re-run")); exec_commands(g_cfg.exec, "ini"); }
        cmdfile_poll(); watchdog(); dlssmode_register(); dlssmode_poll(); dlsspreset_poll();
    }
}
/* one-shot dump of cvars relevant to the one-frame blue tint (CHANGES row 29) */
static void cvars_dump_once(void)
{
    static int done; void *console; int i;
    static const char *names[] = { "r_ShadersAsyncCompiling", "r_ShadersAsyncActivation", "r_ColorGrading", "r_HDRRendering", "r_MotionBlur",
                                   "r_Sharpening", "r_SunShafts", "r_Flares", "r_PostProcessEffects", "r_ShadersPrecache" };
    if (done || !g_wh_base) return;
    console = *(void **)(g_wh_base + RVA_CONSOLE);
    if (!console || !readable((BYTE *)console, 8) || !readable(*(BYTE **)console, 8 * 24)) return;
    done = 1;
    LOG(log_s("cvars:");
        for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++) { void *cv = ((PFN_GetCVar)VT(console, 23))(console, names[i]); log_s(" "); log_s(names[i]); log_s("="); if (cv) log_i(cvar_int(cv)); else log_s("?"); });
}
static float mip_bias_now(void)
{
    BYTE *r; UINT rw = 0, nw = 0;
    if (!g_wh_base || !g_cfg.mipbias) return 0;
    r = g_wh_base + RVA_RENDERER;
    rw = *(UINT *)(r + R_RENDER_W); nw = *(UINT *)(r + R_NATIVE_W);
    if (!rw || !nw || rw >= nw) {                            /* renderer sizes not computed yet (or native still = render): ask the cvars */
        cvars_lookup();
        rw = (UINT)cvar_int(g_cv_width); nw = (UINT)cvar_int(g_cv_native);
        if (!nw && cvar_int(g_cv_fsnative)) nw = (UINT)GetSystemMetrics(SM_CXSCREEN);
    }
    if (!rw || !nw || rw >= nw || rw < 64 || nw > 16384) return 0;
    return log2f_approx((float)rw / (float)nw) + g_cfg.mipoff;
}

static HRESULT STDMETHODCALLTYPE my_CreateSamplerState(ID3D11Device *dev, const D3D11_SAMPLER_DESC *desc, ID3D11SamplerState **out)
{
    g_css_total++;   /* 0.6.0: samplers are created unbiased; the bias is applied at bind time (sampler cache below) */
    return g_real_css(dev, desc, out);
}

/* ---- sampler cache: one biased twin per game sampler, re-made when the bias changes. Draws into a native-sized render
 * target (everything after the upscale: UI, final composite) keep the original samplers (vendors: do not bias UI/post
 * samplers). The game's own state cache compares its own pointers, so the swap is invisible to it; our state restore
 * re-binds what it read back (already biased twins), which the lookup passes through unchanged. */
#define SC_MAX 512
static struct { ID3D11SamplerState *orig, *biased; float bias; } g_sc[SC_MAX];
static int g_sc_n; static CRITICAL_SECTION g_sc_cs; static int g_sc_cs_init;
static float g_sc_bias;          /* bias in effect this frame (0 = none) */
static int g_rt_native;          /* the bound render target is native-sized (post-upscale / UI) */
static unsigned int g_sc_subst;  /* biased bind calls (diagnostic) */
typedef void (STDMETHODCALLTYPE *PFN_SetSamplers)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
typedef void (STDMETHODCALLTYPE *PFN_OMSetRT)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
typedef void (STDMETHODCALLTYPE *PFN_OMSetRTUAV)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *, UINT, UINT, ID3D11UnorderedAccessView *const *, const UINT *);
/* 0.6.6: NVIDIA's driver gives every immediate context its own (heap) vtable, so the originals are kept per vtable: hooking device #1's
 * context did nothing for the renderer's context (fresh-install test run058). */
#define CTXH_MAX 12
static struct { void **vt; PFN_SetSamplers ss[6]; PFN_OMSetRT omrt; PFN_OMSetRTUAV omrtuav; } g_ctxh[CTXH_MAX]; static int g_ctxh_n;
static int ctxh_find(ID3D11DeviceContext *c) { void **vt = *(void ***)c; int i; for (i = 0; i < g_ctxh_n; i++) if (g_ctxh[i].vt == vt) return i; return 0; }
static const int SS_SLOT[6] = { 10, 26, 32, 61, 65, 70 };   /* ID3D11DeviceContext: PS, VS, GS, HS, DS, CS SetSamplers */

static ID3D11SamplerState *sc_get(ID3D11DeviceContext *ctx, ID3D11SamplerState *s, float bias)
{
    int i; D3D11_SAMPLER_DESC d; ID3D11Device *dev = NULL; ID3D11SamplerState *b = NULL; HRESULT hr;
    for (i = 0; i < g_sc_n; i++) {
        if (g_sc[i].biased == s) return s;   /* already a twin */
        if (g_sc[i].orig == s) break;
    }
    if (i < g_sc_n && g_sc[i].biased && g_sc[i].bias == bias) return g_sc[i].biased;
    ID3D11SamplerState_GetDesc(s, &d);
    d.MipLODBias += bias;
    if (d.MipLODBias < -16.0f) d.MipLODBias = -16.0f;
    if (d.MipLODBias > 15.99f) d.MipLODBias = 15.99f;
    ID3D11DeviceContext_GetDevice(ctx, &dev);
    if (!dev) return s;
    {   /* 0.7.5 (BUG-2): with ReShade the game's device is a proxy whose vtable we hooked (g_real_css = the proxy's CreateSamplerState),
         * but a context hooked later (the real one, from dlss_frame) reports the REAL device here: calling the proxy's function on it crashed
         * at the first upscaled frame. Use the device's own vtable entry, skipping our hook when it is ours (the twin is counted once). */
        void **dvt = *(void ***)dev;
        hr = (g_real_css && dvt[23] == (void *)my_CreateSamplerState) ? g_real_css(dev, &d, &b) : ((PFN_CreateSamplerState)dvt[23])(dev, &d, &b);
    }
    REL(dev);
    if (FAILED(hr) || !b) return s;
    g_css_biased++;
    if (i < g_sc_n) { REL(g_sc[i].biased); g_sc[i].biased = b; g_sc[i].bias = bias; return b; }
    if (g_sc_n >= SC_MAX) { REL(b); return s; }
    IUnknown_AddRef((IUnknown *)s);   /* keep the original alive so its pointer is never reused for another sampler */
    g_sc[g_sc_n].orig = s; g_sc[g_sc_n].biased = b; g_sc[g_sc_n].bias = bias; g_sc_n++;
    return b;
}
static void sc_set(int stage, ID3D11DeviceContext *ctx, UINT start, UINT n, ID3D11SamplerState *const *pp)
{
    ID3D11SamplerState *sub[16]; UINT i; float bias = g_sc_bias;
    int h = ctxh_find(ctx);
    if (bias == 0 || g_rt_native || !pp || n == 0 || n > 16) { g_ctxh[h].ss[stage](ctx, start, n, pp); return; }
    EnterCriticalSection(&g_sc_cs);
    for (i = 0; i < n; i++) sub[i] = pp[i] ? sc_get(ctx, pp[i], bias) : NULL;
    LeaveCriticalSection(&g_sc_cs);
    g_sc_subst++;
    g_ctxh[h].ss[stage](ctx, start, n, sub);
}
static void STDMETHODCALLTYPE my_PSSetSamplers(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11SamplerState *const *p) { sc_set(0, c, s, n, p); }
static void STDMETHODCALLTYPE my_VSSetSamplers(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11SamplerState *const *p) { sc_set(1, c, s, n, p); }
static void STDMETHODCALLTYPE my_GSSetSamplers(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11SamplerState *const *p) { sc_set(2, c, s, n, p); }
static void STDMETHODCALLTYPE my_HSSetSamplers(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11SamplerState *const *p) { sc_set(3, c, s, n, p); }
static void STDMETHODCALLTYPE my_DSSetSamplers(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11SamplerState *const *p) { sc_set(4, c, s, n, p); }
static void STDMETHODCALLTYPE my_CSSetSamplers(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11SamplerState *const *p) { sc_set(5, c, s, n, p); }
static void rt_track(ID3D11RenderTargetView *const *rtvs, UINT n)
{
    ID3D11Resource *res = NULL; D3D11_TEXTURE2D_DESC d; UINT nw, nh; int native = 0;
    if (!rtvs || !n || !rtvs[0] || !g_wh_base) { g_rt_native = 0; return; }
    nw = *(UINT *)(g_wh_base + RVA_RENDERER + R_NATIVE_W); nh = *(UINT *)(g_wh_base + RVA_RENDERER + R_NATIVE_H);
    ID3D11RenderTargetView_GetResource(rtvs[0], &res);
    if (res && tex_desc(res, &d) && nw && d.Width == nw && d.Height == nh) native = 1;
    REL(res);
    g_rt_native = native;
}
static void STDMETHODCALLTYPE my_OMSetRT(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *ds) { rt_track(rt, n); frame_tick(); g_ctxh[ctxh_find(c)].omrt(c, n, rt, ds); }
static void STDMETHODCALLTYPE my_OMSetRTUAV(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *ds, UINT us, UINT nu, ID3D11UnorderedAccessView *const *uav, const UINT *ic)
{ if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) rt_track(rt, n); g_ctxh[ctxh_find(c)].omrtuav(c, n, rt, ds, us, nu, uav, ic); }
static const WCHAR *mod_name(ULONG_PTR addr, ULONG_PTR *base, WCHAR *buf, int n);
static void log_mod(ULONG_PTR a) { ULONG_PTR base; WCHAR nm[MAX_PATH]; const WCHAR *bn = mod_name(a, &base, nm, MAX_PATH); if (bn) { log_w(bn); log_s("+"); log_x(a - base); } else log_x(a); }
static void hook_ctx(ID3D11DeviceContext *ctx)
{
    void **vt; DWORD old, tmp; int i;
    int slot, rehook = 0; static int rehooks[CTXH_MAX], limited[CTXH_MAX], logs[CTXH_MAX]; static DWORD win_t0[CTXH_MAX];
    if (!ctx) return;
    vt = *(void ***)ctx;
    if (!vt) return;
    for (slot = 0; slot < g_ctxh_n; slot++) if (g_ctxh[slot].vt == vt) break;
    if (slot < g_ctxh_n && vt[33] == (void *)my_OMSetRT) return;   /* this vtable is hooked already */
    /* 0.7.1: slot < g_ctxh_n with a foreign vt[33] = the driver freed a context's vtable and gave the address to a new one (seen in
     * run 064: the render device's context landed on a "hooked" address, no tick, no DLSS): hook it again in the same slot */
    if (slot == g_ctxh_n && g_ctxh_n >= CTXH_MAX) return;
    rehook = slot < g_ctxh_n;
    if (rehook) {   /* 0.7.5: the D3D11 runtime puts its own functions back into a context vtable now and then (once or twice after creation
                     * on every machine; EVERY frame for the real context behind ReShade's proxy, run069/070, while the game binds through the
                     * proxy anyway). Re-hook, but at most 10 times per 10 s per vtable; the first 3 are logged with the current owners. */
        DWORD now = GetTickCount();
        if ((int)(now - win_t0[slot]) > 10000) { win_t0[slot] = now; rehooks[slot] = 0; }
        if (++rehooks[slot] <= 3 && ++logs[slot] <= 3) LOG(log_s("sampler cache: context vtable "); log_x((ULONG_PTR)vt); log_s(" lost our hooks: OMSetRenderTargets now "); log_mod((ULONG_PTR)vt[33]); log_s(", PSSetSamplers "); log_mod((ULONG_PTR)vt[10]); log_s(" (re-hook "); log_i(logs[slot]); log_s(")"));
        if (rehooks[slot] > 10) { if (!limited[slot]) { limited[slot] = 1; LOG(log_s("sampler cache: context vtable "); log_x((ULONG_PTR)vt); log_s(" is rewritten every frame (a proxy's real context?): re-hooking at most 10x per 10 s")); } return; }
    }
    if (!g_sc_cs_init) { InitializeCriticalSection(&g_sc_cs); g_sc_cs_init = 1; }
    if (!VirtualProtect(&vt[10], 61 * sizeof(void *), PAGE_READWRITE, &old)) { LOG(log_s("cannot write the context vtable: no mip bias")); return; }
    g_ctxh[slot].vt = vt;
    for (i = 0; i < 6; i++) g_ctxh[slot].ss[i] = (PFN_SetSamplers)vt[SS_SLOT[i]];
    g_ctxh[slot].omrt = (PFN_OMSetRT)vt[33]; g_ctxh[slot].omrtuav = (PFN_OMSetRTUAV)vt[34];
    if (slot == g_ctxh_n) g_ctxh_n++;
    vt[10] = (void *)my_PSSetSamplers; vt[26] = (void *)my_VSSetSamplers; vt[32] = (void *)my_GSSetSamplers;
    vt[61] = (void *)my_HSSetSamplers; vt[65] = (void *)my_DSSetSamplers; vt[70] = (void *)my_CSSetSamplers;
    vt[33] = (void *)my_OMSetRT; vt[34] = (void *)my_OMSetRTUAV;
    VirtualProtect(&vt[10], 61 * sizeof(void *), old, &tmp);
    LOG(log_s("sampler cache: context vtable "); log_x((ULONG_PTR)vt); log_s(rehook ? " RE-hooked, address reused (#" : " hooked (#"); log_i(slot + 1); log_s(", 6 SetSamplers stages + OMSetRenderTargets): mip bias applied at bind time"));
}

static void hook_sampler_state(ID3D11Device *dev)
{
    void **vt = *(void ***)dev; DWORD old, tmp;
    if (!vt || g_css_vtable == vt) return;
    if (g_css_vtable) { LOG(log_s("second device vtable "); log_x((ULONG_PTR)vt); log_s(": mip bias hook not installed there")); return; }
    if (vt[23] == (void *)my_CreateSamplerState) { g_css_vtable = vt; return; }
    if (!VirtualProtect(&vt[23], sizeof(void *), PAGE_READWRITE, &old)) { LOG(log_s("cannot write the device vtable: no mip bias")); return; }
    g_real_css = (PFN_CreateSamplerState)vt[23];
    vt[23] = (void *)my_CreateSamplerState;
    VirtualProtect(&vt[23], sizeof(void *), old, &tmp);
    g_css_vtable = vt;
    LOG(log_s("CreateSamplerState hooked (device vtable "); log_x((ULONG_PTR)vt); log_s("): mip bias active once the sizes are known"));
}

static void hook_stretch(BYTE *self, BYTE draw)
{
    g_orig_stretch(self, draw);
    if (!draw || g_dl.disabled || g_no_upscaler || !g_ngx.core || !g_cfg.dlss) { g_sc_bias = 0; return; }
    if (self != g_wh_base + RVA_RENDERER) { dl_fail("stretch called on an unexpected renderer object"); return; }
    if (*(int *)(self + R_SUPERSAMPLE) > 1) { g_sc_bias = 0; return; }                    /* the game's downsample path */
    if (*(UINT *)(self + R_RENDER_W) == *(UINT *)(self + R_NATIVE_W) && *(UINT *)(self + R_RENDER_H) == *(UINT *)(self + R_NATIVE_H)) { g_sc_bias = 0; return; }
    dlss_frame(self);
}

/* $VelocityObjects is created as eTF 0x0b R8G8S (8-bit, sqrt-encoded): 1-2 px error on a 1 px motion = useless for DLSS
 * (aliasing on moving objects, user 2026-11-08). Change the eTF immediate to 0x0e R16G16F at both creation sites
 * (left/mono 0x182020671 and the stereo right eye 0x1820206bc); the game's encode/decode math is unchanged, just lossless. */
#define RVA_VELFMT_L (g_wh_build->velfmt_l)
#define RVA_VELFMT_R (g_wh_build->velfmt_r)
static void velocity_fmt_patch(BYTE *base)
{
    const BYTE *ctx_l = g_wh_build->ctx_l;
    static const BYTE ctx_r[8]  = { 0x41,0xb1,0x0b, 0x44,0x89,0x74,0x24,0x38 };
    BYTE *l = base + RVA_VELFMT_L, *r = base + RVA_VELFMT_R; DWORD old, tmp; int n = 0;
    if (!readable(l - 20, 23) || !readable(r, 8)) { LOGC(log_s("velocity: patch site unreadable: 8-bit velocity kept")); return; }
    if (l[2] == ETF_R16G16F && r[2] == ETF_R16G16F) { LOG(log_s("velocity: format already patched")); return; }
    if (!same(l - 20, ctx_l, 23) || !same(r, ctx_r, 8)) { LOGC(log_s("velocity: creation-site bytes differ: 8-bit velocity kept")); return; }
    if (VirtualProtect(l + 2, 1, PAGE_EXECUTE_READWRITE, &old)) { l[2] = ETF_R16G16F; VirtualProtect(l + 2, 1, old, &tmp); n++; }
    if (VirtualProtect(r + 2, 1, PAGE_EXECUTE_READWRITE, &old)) { r[2] = ETF_R16G16F; VirtualProtect(r + 2, 1, old, &tmp); n++; }
    FlushInstructionCache(GetCurrentProcess(), l, 1); FlushInstructionCache(GetCurrentProcess(), r, 1);
    LOG(log_s("velocity: $VelocityObjects format eTF 0x0b R8G8S -> 0x0e R16G16F at "); log_i(n); log_s(" creation site(s)"));
}

static void dlss_install(BYTE *base)
{
    const BYTE *zero_ctx = g_wh_build->zero_ctx;
    static const BYTE nop8[8] = { 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90 };
    BYTE *z = base + RVA_JITTER_ZERO, **slot = (BYTE **)(base + RVA_VT_UPSCALE); DWORD old, tmp;
    g_wh_base = base;
    if (g_cfg.velpatch) velocity_fmt_patch(base); else LOG(log_s("velocity: format patch disabled by KCDDLSS.ini (8-bit velocity kept)"));
    if (!readable(z - 8, 16) || !readable((BYTE *)slot, 8)) { LOGC(log_s("dlss: patch sites unreadable: upscale hook not installed")); return; }
    if (!g_cfg.jitter) LOG(log_s("dlss: jitter patch disabled by KCDDLSS.ini (game keeps zero jitter)"));
    else if (same(z, nop8, 8)) LOG(log_s("dlss: jitter zeroing already patched"));
    else if (!same(z - 8, zero_ctx, 16)) { LOGC(log_s("dlss: jitter generator bytes differ: unsupported game version, upscale hook not installed")); return; }
    else if (!VirtualProtect(z, 8, PAGE_EXECUTE_READWRITE, &old)) { LOGC(log_s("dlss: cannot write the jitter generator: upscale hook not installed")); return; }
    else { memcpy(z, nop8, 8); VirtualProtect(z, 8, old, &tmp); FlushInstructionCache(GetCurrentProcess(), z, 8); LOG(log_s("dlss: jitter zeroing NOPed at WHGame.dll+"); log_x(RVA_JITTER_ZERO)); }
    if (*slot != base + RVA_FN_UPSCALE) { LOG(log_s("dlss: vtable entry +0xab8 is "); log_x((ULONG_PTR)*slot); log_s(", expected WHGame.dll+"); log_x(RVA_FN_UPSCALE); log_s(": upscale hook not installed")); return; }
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { LOGC(log_s("dlss: cannot write the renderer vtable: upscale hook not installed")); return; }
    g_orig_stretch = (PFN_Stretch)(void *)*slot;
    *slot = (BYTE *)(void *)hook_stretch;
    VirtualProtect(slot, 8, old, &tmp);
    LOG(log_s("dlss: upscale hook installed (renderer vtable +0xab8), waiting for the first upscaled frame"));
}

/* --- Disable Forced Conversation Sharpening (Nexus mod 2238 by its author, folded in on the user's request) ---
 * When a dialogue starts the game reads its sharpening cvar (vtable slot 4, GetFVal), keeps the old value and
 * forces 0.7 on it (slot 8, Set(float)); the dialogue end restores the kept value. The mod NOPs the Set call
 * (3 bytes) so the player's sharpening stays. Same byte pattern as the mod's SUWSF.ini, matched once. */
static const BYTE SHARP_SIG[] = {
    0x48,0x8B,0x8D,0x60,0x01,0x00,0x00, 0xF3,0x0F,0x10,0x0D,0,0,0,0, 0xF3,0x0F,0x11,0x85,0x68,0x01,0x00,0x00,
    0x48,0x8B,0x01, 0xFF,0x50,0x40, 0x48,0x8B,0x8D,0x70,0x01,0x00,0x00, 0xBA,0x01,0x00,0x00,0x00, 0x48,0x8B,0x01, 0xFF,0x50,0x38 };
static const BYTE SHARP_MASK[] = {
    1,1,1,1,1,1,1, 1,1,1,1,0,0,0,0, 1,1,1,1,1,1,1,1, 1,1,1, 1,1,1, 1,1,1,1,1,1,1, 1,1,1,1,1, 1,1,1, 1,1,1 };
#define SHARP_LEN ((SIZE_T)sizeof SHARP_SIG)
#define SHARP_OFF 26
static BYTE *g_sharp_base;
static void sharpen_patch(BYTE *base)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base; IMAGE_NT_HEADERS64 *nt; IMAGE_SECTION_HEADER *sec;
    BYTE *hit = NULL, *at; DWORD i, old, tmp; int hits = 0; static const BYTE nop3[3] = { 0x90, 0x90, 0x90 };
    if (!base || base == g_sharp_base) return;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        BYTE *s = base + sec->VirtualAddress, *p, *end; SIZE_T n = sec->Misc.VirtualSize;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (n < SHARP_LEN || !readable(s, n)) continue;
        end = s + n - SHARP_LEN;
        for (p = s; p <= end; p++) {
            SIZE_T k;
            if (p[0] != SHARP_SIG[0] || p[1] != SHARP_SIG[1] || p[2] != SHARP_SIG[2] || p[3] != SHARP_SIG[3]) continue;
            for (k = 4; k < SHARP_LEN; k++) if (SHARP_MASK[k] && p[k] != SHARP_SIG[k]) break;
            if (k == SHARP_LEN) { if (!hits) hit = p; hits++; }
        }
    }
    if (hits != 1) { LOG(log_s("sharpen: code found "); log_u((DWORD)hits); log_s(" times (need exactly 1): unsupported game version, conversation sharpening unchanged")); return; }
    at = hit + SHARP_OFF;
    if (same(at, nop3, 3)) { LOG(log_s("sharpen: already patched at "); log_x((ULONG_PTR)at)); g_sharp_base = base; return; }
    if (!VirtualProtect(at, 3, PAGE_EXECUTE_READWRITE, &old)) { LOGC(log_s("sharpen: cannot write to WHGame.dll's code: conversation sharpening unchanged")); return; }
    memcpy(at, nop3, 3);
    VirtualProtect(at, 3, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at, 3);
    g_sharp_base = base;
    LOG(log_s("sharpen: forced conversation sharpening (Set 0.7) disabled: 3 bytes NOPed at "); log_x((ULONG_PTR)at));
}

/* --- 0.8.0: pak-free menu rows (FINDINGS_KCD_UI_XMLLOAD.md) ---
 * Every UIAction flowgraph XML is parsed through ISystem::LoadXmlFromFile (CSystem vtable slot 112; the Advanced Graphics page
 * is loaded once, in CFlashUI::PostInit). The plugin takes that slot: for MM_AdvancedGraphics.xml it reads the game's own copy
 * through gEnv->pCryPak (so another mod's version of the page gets the rows too), inserts the "DLSS / Upscaling" and "DLSS Model"
 * rows as text (menu_rows.h, generated from tools/menu-mod/MM_AdvancedGraphics.xml) and parses the result with LoadXmlFromBuffer
 * (slot 111). The "Unsupported" choices are added only when no upscaler exists (g_no_upscaler). Anything unexpected -> the
 * original call, the page is the stock one and the mode comes from KCDDLSS.ini. Replaces the Mods\KCDDLSS pak of BETA 1/2. */
#include "menu_rows.h"
#define RVA_CSYS_VT         (g_wh_build->csys_vt)   /* CSystem vtable (ISystem at offset 0, 213 slots) */
#define RVA_FN_LOADXML_FILE (g_wh_build->fn_loadxml_file)    /* slot 112: XmlNodeRef *LoadXmlFromFile(this, XmlNodeRef *sret, const char *file, bool bReuseStrings) */
#define RVA_FN_LOADXML_BUF  (g_wh_build->fn_loadxml_buf)    /* slot 111: XmlNodeRef *LoadXmlFromBuffer(this, XmlNodeRef *sret, const char *buf, size_t size, bool bReuseStrings) */
#define RVA_PCRYPAK         (g_wh_build->pcrypak)   /* ICryPak *gEnv->pCryPak */
#define PAK_SLOT_FOPEN    37   /* FILE *FOpen(name, "rb", flags) */
#define PAK_SLOT_FREAD    40   /* size_t FRead(buf, size, FILE *) -> bytes read; slots as used by the game's read-whole-file helper WHGame+0x237bb0 */
#define PAK_SLOT_FGETSIZE 47
#define PAK_SLOT_FCLOSE   56
#define MENU_XML_NAME "MM_AdvancedGraphics.xml"
typedef void *(*PFN_LoadXmlFile)(void *sys, void **sret, const char *file, BYTE reuse);
typedef void *(*PFN_LoadXmlBuf)(void *sys, void **sret, const char *buf, SIZE_T size, BYTE reuse);
typedef void *(*PFN_PakOpen)(void *pak, const char *name, const char *mode, unsigned flags);
typedef SIZE_T (*PFN_PakRead)(void *pak, void *buf, SIZE_T size, void *f);
typedef SIZE_T (*PFN_PakSize)(void *pak, void *f);
typedef int (*PFN_PakClose)(void *pak, void *f);
static PFN_LoadXmlFile g_orig_loadxml; static PFN_LoadXmlBuf g_loadxml_buf; static int g_menu_xml_calls;
static int ci_eq_n(const char *a, const char *b) { for (;; a++, b++) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; if (!x) return 1; } }
static const char *mem_find(const char *hay, SIZE_T n, const char *needle, SIZE_T from)
{
    SIZE_T k = 0, i; while (needle[k]) k++;
    if (k == 0 || n < k) return NULL;
    for (i = from; i + k <= n; i++) if (hay[i] == needle[0] && same((const BYTE *)hay + i, (const BYTE *)needle, k)) return hay + i;
    return NULL;
}
typedef struct { char *p; SIZE_T n, cap; } SB;
static void sb_mem(SB *b, const char *s, SIZE_T n) { if (b->n + n > b->cap) n = b->cap - b->n; memcpy(b->p + b->n, s, n); b->n += n; }
static void sb_s(SB *b, const char *s) { SIZE_T n = 0; while (s[n]) n++; sb_mem(b, s, n); }
static void sb_u(SB *b, unsigned v) { char t[12]; int i = 11; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); sb_s(b, t + i); }
static void menu_fail(const char *why) { LOG(log_s("menu: " MENU_XML_NAME ": "); log_s(why); log_s(": the game's own page is loaded (no DLSS rows; set the mode in KCDDLSS.ini)")); }
/* returns the parsed node (through *sret) or NULL = let the original call load the stock page */
static void *menu_xml_load(void *sys, void **sret, const char *file, BYTE reuse)
{
    void *pak, *f, *res; char *src, *line; const char *nodes_end, *edges_end, *e, *e_start, *e_end; SIZE_T size, n, i; int unsupp = g_no_upscaler, k, prev;
    SB out = { 0, 0, 0 };
    if (!readable(g_wh_base + RVA_PCRYPAK, 8)) { menu_fail("pCryPak slot unreadable"); return NULL; }
    pak = *(void **)(g_wh_base + RVA_PCRYPAK);
    if (!pak || !readable((const BYTE *)pak, 8) || !readable(*(BYTE **)pak, (PAK_SLOT_FCLOSE + 1) * 8)) { menu_fail("pCryPak not ready"); return NULL; }
    f = ((PFN_PakOpen)VT(pak, PAK_SLOT_FOPEN))(pak, file, "rb", 0);
    if (!f) { menu_fail("pCryPak cannot open the file"); return NULL; }
    size = ((PFN_PakSize)VT(pak, PAK_SLOT_FGETSIZE))(pak, f);
    if (size < 1000 || size > (4u << 20)) { ((PFN_PakClose)VT(pak, PAK_SLOT_FCLOSE))(pak, f); menu_fail("unexpected file size"); return NULL; }
    src = VirtualAlloc(NULL, size + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!src) { ((PFN_PakClose)VT(pak, PAK_SLOT_FCLOSE))(pak, f); menu_fail("out of memory"); return NULL; }
    n = ((PFN_PakRead)VT(pak, PAK_SLOT_FREAD))(pak, src, size, f);
    ((PFN_PakClose)VT(pak, PAK_SLOT_FCLOSE))(pak, f);
    if (n != size) { VirtualFree(src, 0, MEM_RELEASE); menu_fail("short read"); return NULL; }
    src[size] = 0;
    if (size > 4 && (src[0] != '<' || mem_find(src, size, "<Graph", 0) == NULL)) { VirtualFree(src, 0, MEM_RELEASE); menu_fail("not a text XML (binary XML or another layout)"); return NULL; }
    if (mem_find(src, size, "SW_DLSS", 0)) { VirtualFree(src, 0, MEM_RELEASE); menu_fail("already has the DLSS rows (the BETA 1/2 Mods\\KCDDLSS pak is still installed: delete it)"); return NULL; }
    nodes_end = mem_find(src, size, "</Nodes>", 0); edges_end = mem_find(src, size, "</Edges>", 0);
    e = mem_find(src, size, "nodeIn=\"557\" nodeOut=\"824\"", 0);
    if (!nodes_end || !edges_end || !e || !(nodes_end < e && e < edges_end)) { VirtualFree(src, 0, MEM_RELEASE); menu_fail("anchors not found (changed page layout: another mod or game version)"); return NULL; }
    e_start = e; while (e_start > src && e_start[-1] != '\n') e_start--;              /* whole <Edge .../> line incl. indentation */
    e_end = mem_find(src, size, "/>", (SIZE_T)(e - src)); if (!e_end) { VirtualFree(src, 0, MEM_RELEASE); menu_fail("edge end not found"); return NULL; }
    e_end += 2; if (e_end[0] == '\r') e_end++; if (e_end[0] == '\n') e_end++;
    while (nodes_end > src && (nodes_end[-1] == ' ' || nodes_end[-1] == '\t')) nodes_end--;   /* keep the closing tag's indentation after our nodes */
    while (edges_end > src && (edges_end[-1] == ' ' || edges_end[-1] == '\t')) edges_end--;
    out.cap = size + 12288; out.p = VirtualAlloc(NULL, out.cap + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!out.p) { VirtualFree(src, 0, MEM_RELEASE); menu_fail("out of memory"); return NULL; }
    sb_mem(&out, src, (SIZE_T)(nodes_end - src));
    sb_s(&out, MENU_NODES_A); if (unsupp) sb_s(&out, MENU_NODE_UNSUPP_MODE); sb_s(&out, MENU_NODES_B); if (unsupp) sb_s(&out, MENU_NODE_UNSUPP_PRESET);
    sb_mem(&out, nodes_end, (SIZE_T)(e_start - nodes_end));
    for (k = 1, prev = MENU_CHAIN_IDS[0]; k < (int)(sizeof MENU_CHAIN_IDS / sizeof MENU_CHAIN_IDS[0]); k++) {
        int id = MENU_CHAIN_IDS[k];
        if (!unsupp && (id == MENU_UNSUPP_MODE || id == MENU_UNSUPP_PRESET)) continue;
        sb_s(&out, "    <Edge nodeIn=\""); sb_u(&out, (unsigned)id); sb_s(&out, "\" nodeOut=\""); sb_u(&out, (unsigned)prev); sb_s(&out, "\" portIn=\"Call\" portOut=\"OnCall\" enabled=\"1\" />\r\n");
        prev = id;
    }
    sb_mem(&out, e_end, (SIZE_T)(edges_end - e_end));
    sb_s(&out, MENU_EDGES_IO);
    sb_mem(&out, edges_end, size - (SIZE_T)(edges_end - src));
    out.p[out.n] = 0; i = out.n;
    VirtualFree(src, 0, MEM_RELEASE);
#ifdef KCDDLSS_DEBUG
    {   /* debug build: the patched page next to the log, for offline validation (tools/menu-mod/verify_menu_mod.py) */
        WCHAR path[MAX_PATH + 40]; HANDLE h; DWORD w; path[0] = 0; wcat(path, MAX_PATH + 40, g_dir); wcat(path, MAX_PATH + 40, L"\\KCDDLSS-menu.xml");
        h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { WriteFile(h, out.p, (DWORD)out.n, &w, NULL); CloseHandle(h); }
    }
#endif
    line = out.p;
    res = g_loadxml_buf(sys, sret, line, i, reuse);   /* the buffer stays allocated: the parser may keep string references (bReuseStrings) */
    LOG(log_s("menu: " MENU_XML_NAME " patched in memory: "); log_u((DWORD)size); log_s(" -> "); log_u((DWORD)i); log_s(" bytes, DLSS / Upscaling + DLSS Model rows");
        log_s(unsupp ? " with the Unsupported choices (no upscaler on this system)" : ""); log_s(res && *sret ? "; parsed OK (no Mods\\KCDDLSS pak needed)" : "; PARSE FAILED: the game's own page is loaded instead"));
    if (!res || !*sret) return NULL;
    return res;
}
static void *my_LoadXmlFile(void *sys, void **sret, const char *file, BYTE reuse)
{
    const char *bn, *q; void *res;
    if (file && g_wh_base) {
        for (bn = q = file; *q; q++) if (*q == '/' || *q == '\\') bn = q + 1;
        if (ci_eq_n(bn, MENU_XML_NAME)) {
            if (g_menu_xml_calls++ < 3) LOG(log_s("menu: LoadXmlFromFile(\""); log_s(file); log_s("\") call "); log_i(g_menu_xml_calls); log_s(": patching"));
            res = menu_xml_load(sys, sret, file, reuse);
            if (res) return res;
        }
    }
    return g_orig_loadxml(sys, sret, file, reuse);
}
static void menu_xml_install(BYTE *base)
{
    BYTE **slot = (BYTE **)(base + RVA_CSYS_VT + 112 * 8), **slot_buf = (BYTE **)(base + RVA_CSYS_VT + 111 * 8); DWORD old, tmp;
    if (!g_cfg.menurows) { LOG(log_s("menu: rows disabled by KCDDLSS.ini (MenuRows 0): the page is the game's own")); return; }
    if (!readable((BYTE *)slot_buf, 16)) { LOGC(log_s("menu: CSystem vtable unreadable: no DLSS rows in the options menu")); return; }
    if (*slot == (BYTE *)my_LoadXmlFile) return;
    if (*slot != base + RVA_FN_LOADXML_FILE || *slot_buf != base + RVA_FN_LOADXML_BUF) {
        LOGC(log_s("menu: ISystem::LoadXmlFromFile slot is "); log_x((ULONG_PTR)*slot); log_s(", expected WHGame.dll+"); log_x(RVA_FN_LOADXML_FILE); log_s(": unsupported game version, no DLSS rows in the options menu")); return;
    }
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { LOGC(log_s("menu: cannot write the CSystem vtable: no DLSS rows in the options menu")); return; }
    g_orig_loadxml = (PFN_LoadXmlFile)(void *)*slot; g_loadxml_buf = (PFN_LoadXmlBuf)(void *)*slot_buf;
    *slot = (BYTE *)my_LoadXmlFile;
    VirtualProtect(slot, 8, old, &tmp);
    LOG(log_s("menu: ISystem::LoadXmlFromFile hooked (CSystem vtable slot 112): the DLSS rows are added to " MENU_XML_NAME " in memory when the UI loads"));
}

/* --- 0.8.0: the renderer's own swap-chain repair, tried as a replacement for the minimize/restore kick (FINDINGS_KCD_NATIVERES_SWITCH.md) ---
 * The game's focus handler DeviceInfo::OnActivate(DI, wParam, lParam) queues event 13; the next RT_BeginFrame then calls the
 * renderer's activate routine (swap chain reset + OnD3D11PostCreateDevice = what alt-tab repairs with). With the window-thread
 * "active" byte DI+0x1e5 preset, OnActivate skips its own ShowWindow and only queues the event. Tested on the PC (runs 076/077,
 * 1080p exclusive fullscreen): the swap chain IS rebuilt, but the routine minimizes the game window as well (both with a
 * deactivate+activate pair and with the activate transition alone), so it has no advantage over the kick. Kept only behind the
 * "kick" command of KCDDLSS.cmd for experiments; the automatic kick stays the 0.7.5 minimize/restore. */
#define RVA_FN_ONACTIVATE (g_wh_build->fn_onactivate)
#define R_DEVINFO   0x29968
#define DI_ACTIVE   0x1e5   /* window-thread copy of the active state; DI+0x1e4 = the render thread's copy */
#define R_FULLSCREEN 0x29c30
static const BYTE ONACTIVATE_SIG[] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x57, 0x48,0x83,0xEC,0x40, 0x66,0x85,0xD2 };
typedef void (*PFN_OnActivate)(BYTE *di, UINT_PTR wp, UINT_PTR lp);
static int kick_activate(const char *why)
{
    BYTE *r, *di, *fn; PFN_OnActivate oa;
    if (!g_wh_base) return 0;
    r = g_wh_base + RVA_RENDERER; di = r + R_DEVINFO; fn = g_wh_base + RVA_FN_ONACTIVATE;
    if (!readable(fn, sizeof ONACTIVATE_SIG) || !same(fn, ONACTIVATE_SIG, sizeof ONACTIVATE_SIG)) { LOG(log_s("display: "); log_s(why); log_s(": OnActivate bytes differ (unsupported game version), renderer re-activate skipped")); return 0; }
    if (!readable(di, 0x300) || !readable(r + R_FULLSCREEN, 4)) { LOG(log_s("display: renderer device info unreadable, re-activate skipped")); return 0; }
    oa = (PFN_OnActivate)(void *)fn;
    /* one transition only: the queue handler calls the renderer's activate routine when the event's state differs from its own
     * copy DI+0x1e4 (set to "inactive" here), so only the "activate" half runs; a deactivate half would minimize the window (run 0.8.0 test). */
    di[DI_ACTIVE - 1] = 0; di[DI_ACTIVE] = 1; oa(di, 1, 0);
    LOG(log_s("display: "); log_s(why); log_s(": renderer re-activate queued (the game's own alt-tab repair: swap chain reset + render-target stack rebuilt next frame, no window messages); fullscreen flag "); log_u(r[R_FULLSCREEN]));
    return 1;
}

static int select_build(BYTE *base)
{
    unsigned int i;
    for (i = 0; i < sizeof g_wh_builds / sizeof g_wh_builds[0]; i++) {
        const WH_BUILD *b = &g_wh_builds[i]; int score = 0;
        BYTE **up = (BYTE **)(base + b->vt_upscale), **cs = (BYTE **)(base + b->csys_vt);
        if (readable((BYTE *)up, 8) && *up == base + b->fn_upscale) score++;
        if (readable((BYTE *)cs, 113 * 8) && cs[111] == base + b->fn_loadxml_buf && cs[112] == base + b->fn_loadxml_file) score++;
        if (score) {
            g_wh_build = b;
            LOG(log_s("WHGame.dll build: "); log_s(b->name); log_s(" ("); log_i(score);
                log_s(score == 2 ? "/2 identity checks)" : "/2 identity checks: partial match, the other vtable is probably hooked by another mod)"));
            return 1;
        }
    }
    LOGC(log_s("WHGame.dll build: unknown (neither Steam 1.9.8 nor Epic 1.9.8b): DLSS, velocity, jitter and menu patches skipped"));
    return 0;
}

static void hook_whgame(BYTE *base)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base; IMAGE_NT_HEADERS64 *nt; IMAGE_DATA_DIRECTORY *dir; IMAGE_IMPORT_DESCRIPTOR *imp;
    FARPROC real_gpa; int done = 0, known;
    if (!base || base == g_hooked_base) return;
    LOG(log_s("WHGame.dll at "); log_x((ULONG_PTR)base));
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { LOGC(log_s("not a PE image: nothing changed")); return; }
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { LOGC(log_s("not a 64-bit PE image: nothing changed")); return; }
    if (nt->OptionalHeader.SizeOfImage < 0x3800000) { LOGC(log_s("WHGame.dll image too small for this build's offsets: nothing changed")); return; }
    known = select_build(base);
    grass_patch(base);
    sharpen_patch(base);
    if (known) {
        dlss_install(base);
        menu_xml_install(base);
    }
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) { LOGC(log_s("no import table: DLSS hook not installed")); return; }
    real_gpa = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress");
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
        const char *dll = (const char *)(base + imp->Name); IMAGE_THUNK_DATA64 *orig, *iat;
        if (!strieq_a(dll, "kernel32.dll")) continue;
        orig = (IMAGE_THUNK_DATA64 *)(base + imp->OriginalFirstThunk); iat = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; orig->u1.AddressOfData; orig++, iat++) {
            IMAGE_IMPORT_BY_NAME *ibn; DWORD old, tmp;
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
            if (!streq((const char *)ibn->Name, "GetProcAddress")) continue;
            if ((FARPROC)(void *)iat->u1.Function != real_gpa) {
                LOG(log_s("WHGame.dll's GetProcAddress import already points elsewhere ("); log_x((ULONG_PTR)iat->u1.Function); log_s("): nothing changed")); return;
            }
            if (!VirtualProtect(&iat->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old)) { LOGC(log_s("cannot write the import table: nothing changed")); return; }
            g_real_gpa = (FARPROC (WINAPI *)(HMODULE, LPCSTR))(void *)real_gpa;
            iat->u1.Function = (ULONGLONG)(ULONG_PTR)(void *)my_GetProcAddress;
            VirtualProtect(&iat->u1.Function, sizeof(ULONGLONG), old, &tmp);
            done = 1; g_hooked_base = base;
            LOG(log_s("GetProcAddress import hooked at WHGame.dll+"); log_x((ULONG_PTR)((BYTE *)&iat->u1.Function - base)); log_s(": waiting for D3D11CreateDevice"));
            break;
        }
        if (done) break;
    }
    if (!done) LOGC(log_s("GetProcAddress import not found in WHGame.dll: nothing changed"));
}

/* ---- wait for WHGame.dll when the plugin is loaded before it ---- */
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } USTR;
typedef struct { ULONG Flags; const USTR *FullDllName; const USTR *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } DLL_NOTE;
typedef VOID (CALLBACK *DLL_NOTE_FN)(ULONG reason, const DLL_NOTE *data, PVOID ctx);
typedef LONG (NTAPI *REGISTER_FN)(ULONG flags, DLL_NOTE_FN fn, PVOID ctx, PVOID *cookie);

static int is_whgame(const USTR *n)
{
    static const WCHAR want[] = L"whgame.dll"; USHORT i, len;
    if (!n || !n->Buffer) return 0;
    len = n->Length / sizeof(WCHAR);
    if (len != 10) return 0;
    for (i = 0; i < len; i++) { WCHAR c = n->Buffer[i]; if (c >= L'A' && c <= L'Z') c = (WCHAR)(c + 32); if (c != want[i]) return 0; }
    return 1;
}
static VOID CALLBACK on_dll(ULONG reason, const DLL_NOTE *d, PVOID ctx)
{
    (void)ctx;
    if (!d || !is_whgame(d->BaseDllName)) return;
    if (reason == 1) hook_whgame((BYTE *)d->DllBase);
    else if (reason == 2) { if ((BYTE *)d->DllBase == g_hooked_base) g_hooked_base = NULL; if ((BYTE *)d->DllBase == g_grass_base) g_grass_base = NULL; }
}

/* 0.7.5 crash line: a first-chance hardware fault (access violation and friends) is logged once with the faulting module and the
 * return addresses on the stack that fall inside a module, so a "the game just closes" report names the place (BUG-2: ReShade). */
static const WCHAR *mod_name(ULONG_PTR addr, ULONG_PTR *base, WCHAR *buf, int n)
{
    HMODULE m = NULL; int i, k;
    *base = 0; buf[0] = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m) || !m) return NULL;
    *base = (ULONG_PTR)m;
    k = (int)GetModuleFileNameW(m, buf, (DWORD)n); if (k <= 0 || k >= n) return NULL;
    for (i = k - 1; i > 0 && buf[i - 1] != L'\\'; i--) ;
    return buf + i;
}
static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep)
{
    static LONG once; DWORD code; ULONG_PTR pc, base, *sp, v; WCHAR nm[MAX_PATH]; const WCHAR *bn; int i, shown = 0;
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    code = ep->ExceptionRecord->ExceptionCode;
    if (code != 0xC0000005 && code != 0xC000001D && code != 0xC0000094 && code != 0xC00000FD && code != 0xC0000409 && code != 0xC0000096) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
    pc = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
    if (TryEnterCriticalSection(&g_log_cs)) {
        log_s("CRASH: exception "); log_x(code); log_s(" at "); log_x(pc);
        bn = mod_name(pc, &base, nm, MAX_PATH); if (bn) { log_s(" = "); log_w(bn); log_s("+"); log_x(pc - base); }
        if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) { log_s(ep->ExceptionRecord->ExceptionInformation[0] ? " writing " : " reading "); log_x(ep->ExceptionRecord->ExceptionInformation[1]); }
        log_s("; stack:");
        sp = (ULONG_PTR *)ep->ContextRecord->Rsp;
        for (i = 0; i < 200 && shown < 14; i++) {
            if (!readable((BYTE *)(sp + i), 8)) break;
            v = sp[i];
            if (v < 0x10000 || !readable((BYTE *)v, 1)) continue;
            bn = mod_name(v, &base, nm, MAX_PATH); if (!bn) continue;
            log_s(" "); log_w(bn); log_s("+"); log_x(v - base); shown++;
        }
        log_end(); LeaveCriticalSection(&g_log_cs);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE pin, wh, ntdll; REGISTER_FN reg; PVOID cookie = NULL;
        DisableThreadLibraryCalls(self);
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void *)on_dll, &pin);
        InitializeCriticalSection(&g_log_cs);
        log_init(self);
        AddVectoredExceptionHandler(1, crash_handler);
        LOGC(log_s(PLUGIN_TITLE));
        cfg_load();
        ntdll = GetModuleHandleW(L"ntdll.dll");
        reg = ntdll ? (REGISTER_FN)(void *)GetProcAddress(ntdll, "LdrRegisterDllNotification") : NULL;
        if (!reg || reg(0, on_dll, NULL, &cookie) != 0) reg = NULL;
        wh = GetModuleHandleW(L"WHGame.dll");
        if (wh) hook_whgame((BYTE *)wh);
        else if (reg) LOG(log_s("WHGame.dll not loaded yet: waiting for it"));
        else LOGC(log_s("WHGame.dll not loaded and no load notification available: nothing changed"));
    }
    return TRUE;
}
