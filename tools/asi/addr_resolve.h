/* addr_resolve.h: run-time address resolver for WHGame.dll (ADDR_CONTRACT.md). Shared by kcd_dlss.c and the offline
 * test addr_test.c; no C runtime, no logging. The includer provides <windows.h>, the addr_sig.h types (struct addr_ent,
 * AK_*) and ADDR_READABLE(p, n) (non-zero = the n bytes at p may be read).
 * addr_run() fills er[i] for every table entry (hits, target or NULL) and out[id] with the voted result (NULL = unresolved). */
struct addr_res { unsigned int hits; BYTE *target; unsigned short next; DWORD q, qm; };   /* q/qm: first 4 sig bytes and their mask (quick reject) */
#define ADDR_MAX_CLS 16
#define ADDR_MAX_CAND 8
static unsigned short addr_head[65536];   /* scan buckets by the first two signature bytes: entry index + 1, chained through .next */
static BYTE addr_bits[8192];              /* the same keys as a bit set (stays in the L1 cache during the scan) */

static int addr_sig_at(const BYTE *p, const struct addr_ent *e)
{
    unsigned k;
    for (k = 0; k < e->len; k++) if ((!e->mask || e->mask[k]) && p[k] != e->sig[k]) return 0;
    return 1;
}
static int addr_cstr_at(const BYTE *p, const BYTE *end, const char *s)   /* NUL-terminated s equals the string at p (incl. its NUL) */
{
    for (;; p++, s++) { if (p >= end || *p != (BYTE)*s) return 0; if (!*s) return 1; }
}
static int addr_sec(BYTE *base, SIZE_T image, const IMAGE_SECTION_HEADER *s, BYTE **b, BYTE **e)   /* section bounds, clamped to the image */
{
    SIZE_T sz = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
    if (!s->VirtualAddress || s->VirtualAddress >= image) return 0;
    if (sz > image - s->VirtualAddress) sz = image - s->VirtualAddress;
    if (sz < 32 || !ADDR_READABLE(base + s->VirtualAddress, sz)) return 0;
    *b = base + s->VirtualAddress; *e = *b + sz;
    return 1;
}
static void addr_run(BYTE *base, const struct addr_ent *tab, unsigned n, struct addr_res *er, BYTE **out, unsigned nid)
{
    IMAGE_NT_HEADERS64 *nt; IMAGE_SECTION_HEADER *sh; SIZE_T image; unsigned i, j, nsec, ncls = 0, c, k;
    struct { const char *name; DWORD td[ADDR_MAX_CAND], col[ADDR_MAX_CAND]; unsigned ntd, ncol, nvt; BYTE *vt; } cls[ADDR_MAX_CLS];
    unsigned char ecls[1024];
    for (i = 0; i < nid; i++) out[i] = NULL;
    for (i = 0; i < n; i++) { er[i].hits = 0; er[i].target = NULL; er[i].next = 0; er[i].q = er[i].qm = 0; }
    if (!base || !ADDR_READABLE(base, 0x400) || ((IMAGE_DOS_HEADER *)base)->e_magic != IMAGE_DOS_SIGNATURE) return;
    if ((DWORD)((IMAGE_DOS_HEADER *)base)->e_lfanew > 0x400 - sizeof(IMAGE_NT_HEADERS64)) return;
    nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    image = nt->OptionalHeader.SizeOfImage; nsec = nt->FileHeader.NumberOfSections; sh = IMAGE_FIRST_SECTION(nt);
    if ((BYTE *)(sh + nsec) > base + 0x1000 || !ADDR_READABLE(sh, nsec * sizeof *sh)) return;
#define ADDR_IN(p, len) ((const BYTE *)(p) >= base && (SIZE_T)((const BYTE *)(p) - base) <= image && image - (SIZE_T)((const BYTE *)(p) - base) >= (SIZE_T)(len))
    /* 1. AK_DATA / AK_CODE: one pass over the code section(s) for all entries together */
    for (i = 0; i < 65536; i++) addr_head[i] = 0;
    for (i = 0; i < 8192; i++) addr_bits[i] = 0;
    for (i = n; i-- > 0;) if ((tab[i].kind == AK_DATA || tab[i].kind == AK_CODE) && tab[i].len >= 2 && tab[i].sig && i < 65535) {
        unsigned key = tab[i].sig[0] | (unsigned)tab[i].sig[1] << 8; er[i].next = addr_head[key]; addr_head[key] = (unsigned short)(i + 1); addr_bits[key >> 3] |= (BYTE)(1u << (key & 7));
        for (k = 0; k < 4 && k < tab[i].len; k++) if (!tab[i].mask || tab[i].mask[k]) { er[i].q |= (DWORD)tab[i].sig[k] << (8 * k); er[i].qm |= 0xffu << (8 * k); }
    }
    for (j = 0; j < nsec; j++) {
        BYTE *p, *last, *s, *end;
        if (!(sh[j].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !addr_sec(base, image, &sh[j], &s, &end)) continue;
        {
          for (p = s, last = end - 1; p < last; p++) {
              unsigned key = p[0] | (unsigned)p[1] << 8, x;
              if (!(addr_bits[key >> 3] & (1u << (key & 7)))) continue;
              x = addr_head[key];
              while (x) {
                  const struct addr_ent *e = &tab[x - 1];
                  if ((SIZE_T)(end - p) >= e->len && (e->len < 4 || (*(const DWORD *)p & er[x - 1].qm) == er[x - 1].q) && addr_sig_at(p, e)) { if (!er[x - 1].hits++) er[x - 1].target = p; }
                  x = er[x - 1].next;
              }
          } }
    }
    for (i = 0; i < n; i++) if (tab[i].kind == AK_DATA || tab[i].kind == AK_CODE) {
        BYTE *h = er[i].target, *t = NULL; er[i].target = NULL;
        if (er[i].hits != 1 || !h) continue;
        if (tab[i].kind == AK_CODE) t = h + tab[i].off;
        else if (tab[i].off >= 0 && tab[i].off + 4 <= (int)tab[i].len) {
            const BYTE *d = h + tab[i].off; int disp = (int)((DWORD)d[0] | (DWORD)d[1] << 8 | (DWORD)d[2] << 16 | (DWORD)d[3] << 24);
            t = h + tab[i].off + 4 + tab[i].tail + disp;
        }
        if (t && ADDR_IN(t, 1)) er[i].target = t;
    }
    /* 2. AK_VTSLOT / AK_VTFUNC: MSVC x64 RTTI. type descriptor (name at +0x10) -> complete object locator (signature 1, offset 0,
     *    pTypeDescriptor = td RVA, pSelf = own RVA) -> the qword holding the locator's address = vtable[-1]; exactly one vtable */
    for (i = 0; i < n && i < sizeof ecls; i++) {
        ecls[i] = 0xff;
        if ((tab[i].kind != AK_VTSLOT && tab[i].kind != AK_VTFUNC) || !tab[i].cls) continue;
        for (c = 0; c < ncls; c++) { const char *a = cls[c].name, *b = tab[i].cls; while (*a && *a == *b) a++, b++; if (*a == *b) break; }
        if (c == ncls) { if (ncls == ADDR_MAX_CLS) continue; cls[c].name = tab[i].cls; cls[c].ntd = cls[c].ncol = cls[c].nvt = 0; cls[c].vt = NULL; ncls++; }
        ecls[i] = (unsigned char)c;
    }
    if (ncls) {
        int pass;
        for (pass = 0; pass < 3; pass++) for (j = 0; j < nsec; j++) {
            BYTE *p, *s, *end;
            if ((sh[j].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !addr_sec(base, image, &sh[j], &s, &end)) continue;
            {
              if (pass == 0) {          /* type descriptors: 8-aligned, decorated name ".?A..." at +0x10 */
                  for (p = s + 0x10; p + 2 <= end; p += 8) if (p[0] == '.' && p[1] == '?')
                      for (c = 0; c < ncls; c++) if (cls[c].ntd < ADDR_MAX_CAND && addr_cstr_at(p, end, cls[c].name)) cls[c].td[cls[c].ntd++] = (DWORD)(p - 0x10 - base);
              } else if (pass == 1) {   /* complete object locators: 4-aligned, 24 bytes */
                  for (p = s; p + 24 <= end; p += 4) {
                      const DWORD *d = (const DWORD *)p;
                      if (d[0] != 1 || d[1] != 0 || d[5] != (DWORD)(p - base)) continue;
                      for (c = 0; c < ncls; c++) for (k = 0; k < cls[c].ntd; k++) if (d[3] == cls[c].td[k] && cls[c].ncol < ADDR_MAX_CAND) cls[c].col[cls[c].ncol++] = d[5];
                  }
              } else {                  /* vtable[-1]: 8-aligned absolute pointer to a locator */
                  for (p = s; p + 8 <= end; p += 8) {
                      ULONG_PTR q = *(const ULONG_PTR *)p, rva;
                      if (q < (ULONG_PTR)base || q - (ULONG_PTR)base >= image) continue;
                      rva = q - (ULONG_PTR)base;
                      for (c = 0; c < ncls; c++) for (k = 0; k < cls[c].ncol; k++) if (rva == cls[c].col[k]) { if (!cls[c].nvt++) cls[c].vt = p + 8; }
                  }
              } }
        }
        for (i = 0; i < n && i < sizeof ecls; i++) if (ecls[i] != 0xff) {
            BYTE *slot, *f; c = ecls[i];
            er[i].hits = cls[c].nvt;
            if (cls[c].nvt != 1) continue;
            slot = cls[c].vt + (INT_PTR)tab[i].off * 8;   /* off < 0 (not in the contract, emitted by gen_addr.py): qwords before the vtable */
            if (!ADDR_IN(slot, 8)) continue;
            if (tab[i].kind == AK_VTSLOT) { er[i].target = slot; continue; }
            if (!ADDR_READABLE(slot, 8)) continue;
            f = *(BYTE **)slot;
            if (!ADDR_IN(f, tab[i].len ? tab[i].len : 1)) continue;
            if (tab[i].len && tab[i].sig && (!ADDR_READABLE(f, tab[i].len) || !addr_sig_at(f, &tab[i]))) continue;
            er[i].target = f;
        }
    }
#undef ADDR_IN
    /* 3. votes: every entry that resolved votes; the common target when all votes agree, else unresolved */
    for (i = 0; i < nid; i++) {
        BYTE *v = NULL; int bad = 0;
        for (j = 0; j < n; j++) if (tab[j].id == i && er[j].target) { if (!v) v = er[j].target; else if (v != er[j].target) bad = 1; }
        out[i] = bad ? NULL : v;
    }
}
