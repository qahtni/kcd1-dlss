/* addr_test.c: offline proof of the run-time address resolver (addr_resolve.h, the same code kcd_dlss.c runs).
 * Maps a WHGame.dll file at its virtual layout (base relocations applied for wherever the buffer lands), runs
 * 1) the table in addr_sig.h (all ids must resolve to addr_steam_rva on the Steam DLL; reported, not asserted, for a stub table)
 * 2) hand-made entries covering every kind and outcome (asserted). Build: x86_64-w64-mingw32-gcc -O2 -o addr_test.exe addr_test.c
 * Run: wine addr_test.exe path\to\WHGame.dll */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "addr_sig.h"
static int t_readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi; const BYTE *q = p, *end = q + n;
    while (q < end) {
        if (!VirtualQuery(q, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        q = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}
#define ADDR_READABLE(p, n) t_readable((p), (SIZE_T)(n))
#include "addr_resolve.h"

static BYTE *map_dll(const char *path)
{
    FILE *f = fopen(path, "rb"); long sz; BYTE *file, *img; IMAGE_NT_HEADERS64 *nt; IMAGE_SECTION_HEADER *sh; unsigned i; LONGLONG delta;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    file = malloc(sz); if (fread(file, 1, sz, f) != (size_t)sz) return NULL; fclose(f);
    nt = (IMAGE_NT_HEADERS64 *)(file + ((IMAGE_DOS_HEADER *)file)->e_lfanew);
    img = VirtualAlloc((void *)(ULONG_PTR)nt->OptionalHeader.ImageBase, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!img) img = VirtualAlloc(NULL, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!img) return NULL;
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sh = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) if (sh[i].SizeOfRawData)
        memcpy(img + sh[i].VirtualAddress, file + sh[i].PointerToRawData, sh[i].SizeOfRawData < sh[i].Misc.VirtualSize || !sh[i].Misc.VirtualSize ? sh[i].SizeOfRawData : sh[i].Misc.VirtualSize);
    delta = (LONGLONG)(ULONG_PTR)img - (LONGLONG)nt->OptionalHeader.ImageBase;
    if (delta) {   /* IMAGE_REL_BASED_DIR64 only (x64) */
        IMAGE_DATA_DIRECTORY *rd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]; DWORD off = 0, nrel = 0;
        while (off < rd->Size) {
            IMAGE_BASE_RELOCATION *b = (IMAGE_BASE_RELOCATION *)(img + rd->VirtualAddress + off); WORD *e = (WORD *)(b + 1); DWORD k, cnt = (b->SizeOfBlock - sizeof *b) / 2;
            if (!b->SizeOfBlock) break;
            for (k = 0; k < cnt; k++) if ((e[k] >> 12) == IMAGE_REL_BASED_DIR64) { *(ULONGLONG *)(img + b->VirtualAddress + (e[k] & 0xfff)) += delta; nrel++; }
            off += b->SizeOfBlock;
        }
        printf("mapped at %p (not the preferred base): %lu DIR64 relocations applied\n", img, (unsigned long)nrel);
    } else printf("mapped at the preferred base %p\n", img);
    free(file);
    return img;
}
static double now_ms(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)f.QuadPart; }
static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   "); else { printf("  FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

/* hand-made signatures (bytes of the Steam DLL; disp32 wildcarded) */
static const BYTE s_vel[44] = { 0x48,0x8b,0xce,0xe8,0,0,0,0, 0x44,0x8b,0x05,0,0,0,0, 0x48,0x8d,0x05,0,0,0,0, 0x8b,0x15,0,0,0,0, 0x41,0xb1,0x0b, 0x44,0x89,0x74,0x24,0x38, 0x48,0x8b,0xce, 0x48,0x89,0x44,0x24,0x30 };   /* VELFMT_L - 28 */
static const BYTE m_vel[44] = { 1,1,1,1,0,0,0,0, 1,1,1,0,0,0,0, 1,1,1,0,0,0,0, 1,1,0,0,0,0, 1,1,1, 1,1,1,1,1, 1,1,1, 1,1,1,1,1 };
static const BYTE s_none[12] = { 0xde,0xad,0xbe,0xef,0x13,0x37,0xc0,0xde,0xfa,0xce,0xb0,0x0c };
static const BYTE m_all[32] = { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 };
static const BYTE s_many[10] = { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30 };                              /* common prologue */
static const BYTE s_lxf[11] = { 0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x8b,0x89,0xb0,0x08 };                          /* CSystem slot 112 */
static const BYTE s_bad[4] = { 0xcc,0xcc,0xcc,0xcc };
static const struct addr_ent t_tab[] = {
    /* 0 */ { A_RENDERER, AK_DATA, 44, 18, 0, NULL, s_vel, m_vel },          /* lea rax,[rip+x] -> $VelocityObjects 0x378f6d8 */
    /* 1 */ { A_RENDERER, AK_CODE, 12, 0, 0, NULL, s_none, m_all },         /* 0 hits: missing witness, still resolved by #0 */
    /* 2 */ { A_VELFMT_L, AK_CODE, 44, 28, 0, NULL, s_vel, m_vel },          /* -> 0x2020671 */
    /* 3 */ { A_CSYS_VT, AK_VTSLOT, 0, 0, 0, ".?AVCSystem@@", 0, 0 },       /* -> 0x268a6a8 */
    /* 4 */ { A_FN_LOADXML_FILE, AK_VTFUNC, 11, 112, 0, ".?AVCSystem@@", s_lxf, m_all },   /* -> 0xea3d98 with identity check */
    /* 5 */ { A_FN_LOADXML_BUF, AK_VTFUNC, 4, 112, 0, ".?AVCSystem@@", s_bad, m_all },     /* identity check fails: no vote */
    /* 6 */ { A_VT_UPSCALE, AK_VTSLOT, 0, 0xab8 / 8, 0, ".?AVCD3D9Renderer@@", 0, 0 },     /* -> 0x23e9b68 */
    /* 7 */ { A_FN_UPSCALE, AK_VTFUNC, 0, 0xab8 / 8, 0, ".?AVCD3D9Renderer@@", 0, 0 },     /* -> 0x29b51c */
    /* 8 */ { A_JITTER_ZERO, AK_CODE, 10, 0, 0, NULL, s_many, m_all },      /* 2+ hits: no vote */
    /* 9 */ { A_DEPTH_SRV, AK_DATA, 44, 18, 0, NULL, s_vel, m_vel },         /* disagreeing pair: 0x378f6d8 ... */
    /*10 */ { A_DEPTH_SRV, AK_CODE, 44, 28, 0, NULL, s_vel, m_vel },         /* ... vs 0x2020671 -> unresolved */
    /*11 */ { A_STEREO_EYE, AK_VTSLOT, 0, 0, 0, ".?AVNoSuchClass@@", 0, 0 }, /* class not found: no vote */
    /*12 */ { A_SET_VT_LOAD, AK_VTSLOT, 0, -3, 0, ".?AVCUISettings@guimodule@wh@@", 0, 0 },  /* negative slot -> 0x22ecf80 */
    /*13 */ { A_FN_SET_LOAD, AK_VTFUNC, 0, -3, 0, ".?AVCUISettings@guimodule@wh@@", 0, 0 },  /* -> 0x1150d40 */
};
#define T_N (sizeof t_tab / sizeof t_tab[0])

int main(int argc, char **argv)
{
    BYTE *base, *out[A_COUNT]; struct addr_res *er; double t0, t1; unsigned i, ok = 0, match = 0; int stub;
    if (argc < 2 || !(base = map_dll(argv[1]))) { printf("usage: addr_test WHGame.dll\n"); return 2; }
    /* 1. the table in addr_sig.h */
    er = calloc(ADDR_TAB_N, sizeof *er);
    t0 = now_ms(); addr_run(base, addr_tab, ADDR_TAB_N, er, out, A_COUNT); t1 = now_ms();
    for (i = 0; i < A_COUNT; i++) { if (out[i]) ok++; if (out[i] && (DWORD)(out[i] - base) == addr_steam_rva[i]) match++; }
    stub = ADDR_STEAM_STAMP == 0;
    printf("addr_sig.h (%s, %u entries): %u/%u resolved, %u equal addr_steam_rva, %.1f ms\n", stub ? "STUB" : "generated", (unsigned)ADDR_TAB_N, ok, A_COUNT, match, t1 - t0);
    for (i = 0; i < A_COUNT; i++) if (!out[i] || (DWORD)(out[i] - base) != addr_steam_rva[i]) {
        unsigned j; printf("  %-16s %s 0x%x (steam 0x%x):", addr_name[i], out[i] ? "->" : "UNRESOLVED", out[i] ? (unsigned)(out[i] - base) : 0, addr_steam_rva[i]);
        for (j = 0; j < ADDR_TAB_N; j++) if (addr_tab[j].id == i) printf(" [%u] %u hits%s", j, er[j].hits, er[j].target ? "" : " no vote");
        printf("\n");
    }
    {   /* split timing: signature scan entries vs RTTI entries */
        struct addr_ent *sub = calloc(ADDR_TAB_N, sizeof *sub); unsigned k, m, pass;
        for (pass = 0; pass < 2; pass++) {
            for (k = m = 0; k < ADDR_TAB_N; k++) if ((addr_tab[k].kind >= AK_VTSLOT) == (pass == 1)) sub[m++] = addr_tab[k];
            t0 = now_ms(); addr_run(base, sub, m, er, out, A_COUNT); t1 = now_ms();
            printf("  %s entries only (%u): %.1f ms\n", pass ? "RTTI" : "scan", m, t1 - t0);
        }
        addr_run(base, addr_tab, ADDR_TAB_N, er, out, A_COUNT);
    }
    { unsigned k, j, lines = 0;   /* ids the plugin would list in its log (unresolved, or a witness that did not vote for the result) */
      for (k = 0; k < A_COUNT; k++) { int all = out[k] != NULL; for (j = 0; j < ADDR_TAB_N; j++) if (addr_tab[j].id == k && er[j].target != out[k]) all = 0; if (!all) { lines++; printf("  witness missing/disagreeing: %s\n", addr_name[k]); } }
      printf("  plugin log would show %u per-id line(s)\n", lines); }
    if (!stub) { CHECK(match == A_COUNT, "all %u ids resolve to addr_steam_rva", A_COUNT); }
    { IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
      printf("image stamp 0x%08lx size 0x%08lx; table expects 0x%08x / 0x%08x\n", (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage, ADDR_STEAM_STAMP, ADDR_STEAM_IMAGE); }
    /* 2. hand-made entries */
    { struct addr_res te[T_N]; BYTE *o[A_COUNT];
#define RVA(p) ((p) ? (unsigned)((p) - base) : 0u)
      t0 = now_ms(); addr_run(base, t_tab, T_N, te, o, A_COUNT); t1 = now_ms();
      printf("hand-made table (%u entries): %.1f ms\n", (unsigned)T_N, t1 - t0);
      for (i = 0; i < T_N; i++) printf("  [%2u] %-16s kind %d: %u hits, target 0x%x\n", i, addr_name[t_tab[i].id], t_tab[i].kind, te[i].hits, RVA(te[i].target));
      CHECK(te[0].hits == 1 && RVA(te[0].target) == 0x378f6d8u, "AK_DATA lea -> 0x378f6d8");
      CHECK(te[1].hits == 0 && !te[1].target, "0-hit entry gives no vote");
      CHECK(RVA(o[A_RENDERER]) == 0x378f6d8u, "id with one vote + one missing witness resolves");
      CHECK(RVA(o[A_VELFMT_L]) == 0x2020671u, "AK_CODE off 28 -> 0x2020671");
      CHECK(te[3].hits == 1 && RVA(o[A_CSYS_VT]) == 0x268a6a8u, "AK_VTSLOT CSystem slot 0 -> vtable 0x268a6a8");
      CHECK(RVA(o[A_FN_LOADXML_FILE]) == 0xea3d98u, "AK_VTFUNC CSystem slot 112 -> 0xea3d98 (identity bytes match)");
      CHECK(te[5].hits == 1 && !o[A_FN_LOADXML_BUF], "AK_VTFUNC with wrong identity bytes: no vote");
      CHECK(RVA(o[A_VT_UPSCALE]) == 0x23e9b68u, "AK_VTSLOT CD3D9Renderer +0xab8 -> 0x23e9b68");
      CHECK(RVA(o[A_FN_UPSCALE]) == 0x29b51cu, "AK_VTFUNC CD3D9Renderer +0xab8 -> 0x29b51c");
      CHECK(te[8].hits >= 2 && !o[A_JITTER_ZERO], "2+-hit entry (%u hits) gives no vote", te[8].hits);
      CHECK(te[9].target && te[10].target && !o[A_DEPTH_SRV], "disagreeing pair -> unresolved");
      CHECK(te[11].hits == 0 && !o[A_STEREO_EYE], "unknown RTTI class -> no vote");
      CHECK(RVA(o[A_SET_VT_LOAD]) == 0x22ecf80u, "negative slot -3 -> 0x22ecf80");
      CHECK(RVA(o[A_FN_SET_LOAD]) == 0x1150d40u, "negative slot -3 value -> 0x1150d40");
    }
    printf(fails ? "RESULT: %d FAILED\n" : "RESULT: all checks passed\n", fails);
    return fails ? 1 : 0;
}
