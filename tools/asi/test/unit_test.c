/* unit_test.c: the plugin source compiled into a test program (same freestanding flags), so its static functions can be
 * called directly: parsers, ini loading, masked compare, and every HLSL string through D3DCompile.
 * Build + run: see ../test.sh. Exit code = number of failed checks. */
#include "../kcd_dlss.c"
static int t_fail, t_n;
static void t_out(const char *s) { DWORD n = 0, w; while (s[n]) n++; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, NULL); }
static void t_check(int ok, const char *what) { t_n++; if (!ok) { t_fail++; t_out("FAIL  unit: "); t_out(what); t_out("\n"); } }
#define T(x) t_check((x) ? 1 : 0, #x)
static int t_near(float a, float b) { float d = a - b; if (d < 0) d = -d; return d < 0.0005f; }
static void t_shader(PFN_D3DCompile compile, const char *src, size_t len, const char *entry, const char *prof, const char *what)
{
    ID3D10Blob *code = NULL, *err = NULL; HRESULT hr = compile(src, len, what, NULL, NULL, entry, prof, (1 << 15), 0, &code, &err);
    t_n++;
    if (FAILED(hr) || !code) { t_fail++; t_out("FAIL  shader "); t_out(what); t_out(" "); t_out(entry); t_out(": "); if (err) t_out((const char *)ID3D10Blob_GetBufferPointer(err)); t_out("\n"); }
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
}
void unit_entry(void)
{
    static const BYTE a[4] = { 1, 2, 3, 4 }, b[4] = { 1, 9, 3, 4 }, m[4] = { 1, 0, 1, 1 }, m2[4] = { 1, 1, 1, 1 };
    HANDLE h; DWORD w; HMODULE dc; PFN_D3DCompile compile; int v = 0, i; char cmd[64]; const char *c;
    static const char ini[] = "; comment\r\nMode = Performance\r\nGIFullRes = 2 ; trailing\r\ninventoryfix=0\r\nSharpness = 0.5\r\nMipBiasOffset = -0.75\r\nUnknownKey = 7\r\n";
    InitializeCriticalSection(&g_log_cs);
    /* parsers */
    T(streq("abc", "abc")); T(!streq("abc", "abd")); T(!streq("ab", "abc"));
    T(ci_eq("GIFullRes", "gifullres")); T(!ci_eq("GIFullRes2", "gifullres")); T(!ci_eq("GIFull", "gifullres"));
    T(t_near(parse_num("1"), 1)); T(t_near(parse_num(" -0.75"), -0.75f)); T(t_near(parse_num("0.83"), 0.83f)); T(t_near(parse_num("12abc"), 12)); T(t_near(parse_num(""), 0));
    T(mode_parse("Performance") == 0); T(mode_parse("balanced") == 1); T(mode_parse("QUALITY") == 2); T(mode_parse("UltraPerformance") == 3);
    T(mode_parse("UltraQuality") == 4); T(mode_parse("Native") == 5); T(mode_parse("Auto") == -1); T(mode_parse("nonsense") == -2);
    for (i = 0; i < 6; i++) T(mode_parse(MODE_NAMES[i]) == i);
    T(preset_parse("Transformer", &v) == 0); T(preset_parse("cnn", &v) == 1); T(preset_parse("Default", &v) == 2);
    T(preset_parse("k", &v) == 3 && v == 11); T(preset_parse("D", &v) == -1); T(preset_parse("zz", &v) == -1);
    T(mode2menu(4) == 1); T(mode2menu(2) == 2); T(mode2menu(1) == 3); T(mode2menu(0) == 4); T(mode2menu(3) == 5);
    /* masked compare used by the byte-patch guards */
    T(same_m(a, a, m2, 4)); T(!same_m(a, b, m2, 4)); T(same_m(a, b, m, 4));
    /* ini: defaults, then a file in the current directory */
    T(g_cfg.invfix == 1); T(g_cfg.gifull == 1); T(g_cfg.dlss == 1); T(g_cfg.prereq == 1); T(g_cfg.saveres == 1); T(streq(g_cfg.exec, "fxwrap WaterRipples"));
    g_dir[0] = L'.'; g_dir[1] = 0;
    h = CreateFileW(L".\\KCDDLSS.ini", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    T(h != INVALID_HANDLE_VALUE); WriteFile(h, ini, sizeof ini - 1, &w, NULL); CloseHandle(h);
    cfg_load();
    T(g_cfg.mode == 0); T(g_cfg.gifull == 2); T(g_cfg.invfix == 0); T(t_near(g_cfg.sharpness, 0.5f)); T(t_near(g_cfg.mipoff, -0.75f)); T(g_cfg.dlss == 1); T(streq(g_cfg.exec, "fxwrap WaterRipples"));   /* no Exec line: the built-in blue-flash fix stays */
    /* Steam Deck hardware check: this machine is not one; with the registry strings of a Deck it is, and GIFullRes 1 turns off (2 stays) */
    deck_detect(); T(g_deck == 0);
    { static const char K[] = "HARDWARE\\DESCRIPTION\\System\\BIOS"; char mf0[48] = "", pn0[48] = ""; DWORD sz;
      sz = sizeof mf0; RegGetValueA(HKEY_LOCAL_MACHINE, K, "SystemManufacturer", RRF_RT_REG_SZ, NULL, mf0, &sz);
      sz = sizeof pn0; RegGetValueA(HKEY_LOCAL_MACHINE, K, "SystemProductName", RRF_RT_REG_SZ, NULL, pn0, &sz);
      T(RegSetKeyValueA(HKEY_LOCAL_MACHINE, K, "SystemManufacturer", REG_SZ, "Valve", 6) == ERROR_SUCCESS);
      RegSetKeyValueA(HKEY_LOCAL_MACHINE, K, "SystemProductName", REG_SZ, "Galileo", 8); deck_detect(); T(g_deck == 1);
      cfg_load(); T(g_cfg.gifull == 0); T(g_cfg.deckdone == 1);            /* first start on a Deck: 0, written to the ini with the mark */
      g_cfg.deckdone = 0; g_cfg.gifull = 1; cfg_load(); T(g_cfg.gifull == 0); T(g_cfg.deckdone == 1);   /* read back from the file */
      ini_save_key("GIFullRes", "gifullres", "1"); cfg_load(); T(g_cfg.gifull == 1);   /* the player's 1 is kept from then on */
      ini_save_key("GIFullRes", "gifullres", "2"); g_cfg.deckdone = 0; g_cfg.gifull = 1; cfg_load(); T(g_cfg.gifull == 2); T(g_cfg.deckdone == 1);
      RegSetKeyValueA(HKEY_LOCAL_MACHINE, K, "SystemProductName", REG_SZ, "Jupiter", 8); deck_detect(); T(g_deck == 1);
      RegSetKeyValueA(HKEY_LOCAL_MACHINE, K, "SystemProductName", REG_SZ, "MS-7977", 8); deck_detect(); T(g_deck == 0);
      RegSetKeyValueA(HKEY_LOCAL_MACHINE, K, "SystemManufacturer", REG_SZ, mf0, (DWORD)lstrlenA(mf0) + 1);
      RegSetKeyValueA(HKEY_LOCAL_MACHINE, K, "SystemProductName", REG_SZ, pn0, (DWORD)lstrlenA(pn0) + 1); deck_detect(); T(g_deck == 0); }
    /* live commands need the game console: without it they are skipped and change nothing */
    c = "invfix 1"; for (i = 0; c[i]; i++) cmd[i] = c[i]; cmd[i] = 0; exec_commands(cmd, "test"); T(g_cfg.invfix == 0);
    /* no game DLL resolved: every address is "missing" and the accessors say so */
    T(WH(A_RENDERER) == NULL); T(cvar_find("r_Width") == NULL);
    /* every shader the plugin compiles at run time */
    dc = LoadLibraryW(L"d3dcompiler_47.dll"); compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    T(compile != NULL);
    if (compile) {
        t_shader(compile, MV_HLSL, sizeof MV_HLSL - 1, "vs", "vs_5_0", "MV_HLSL"); t_shader(compile, MV_HLSL, sizeof MV_HLSL - 1, "ps", "ps_5_0", "MV_HLSL");
        t_shader(compile, PROBE_HLSL, sizeof PROBE_HLSL - 1, "vs", "vs_5_0", "PROBE_HLSL"); t_shader(compile, PROBE_HLSL, sizeof PROBE_HLSL - 1, "ps", "ps_5_0", "PROBE_HLSL");
        t_shader(compile, DM_HLSL, sizeof DM_HLSL - 1, "vs", "vs_5_0", "DM_HLSL"); t_shader(compile, DM_HLSL, sizeof DM_HLSL - 1, "ps", "ps_5_0", "DM_HLSL");
    }
    t_out(t_fail ? "FAIL  unit test: " : "ok    unit test: "); { char nb[12]; int k = 11, x = t_n - t_fail; nb[k] = 0; do { nb[--k] = (char)('0' + x % 10); x /= 10; } while (x); t_out(nb + k); }
    t_out(" checks passed"); if (t_fail) t_out(" (some FAILED, see above)"); t_out("\n");
    ExitProcess((UINT)t_fail);
}
