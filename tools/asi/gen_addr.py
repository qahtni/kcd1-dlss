#!/usr/bin/env python3
"""Generate addr_sig.h (and addr_sig.json for addr_check.py) from the stock Steam WHGame.dll.
usage: gen_addr.py STOCK_WHGAME_DLL OUT_H        (OUT_H's directory also gets addr_sig.json)

Implements ADDR_CONTRACT.md: every old RVA_* define becomes one id A_*, resolved at run time by voting witnesses
(AK_DATA / AK_CODE masked signatures in .text, AK_VTSLOT / AK_VTFUNC through MSVC RTTI). Everything is proven here on
the Steam DLL with asserts: each signature hits exactly once in .text and resolves to the old RVA, each RTTI entry walks
to the old RVA, each AK_VTFUNC prologue matches. Needs python3 with capstone, pefile, numpy (user site-packages)."""
import sys, os, re, json, struct, bisect, hashlib, time
import numpy as np, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

STEAM_SHA256 = 'cf9f6dc384edcf35c20647a912745ddb8adb5ba65953e329c24e89dd9c4381aa'
MINFIX, MAXLEN, MAXFIX = 20, 72, 48   # fixed bytes wanted / hard caps per signature
WANT = 3                              # witnesses per id
STORE_RX = re.compile(rb'steam|achiev|online|overlay|epic|galaxy|\bgog|\beos|entitle|leaderboard|dlc|cloud|uplay|telemetr', re.I)

# old defines in kcd_dlss.c order: (name, old rva, spec)
#  data            : AK_DATA witnesses from rip-relative references
#  patch lo hi     : AK_CODE windows that must contain the bytes [target+lo, target+hi) (patched + compared by the plugin)
#  func lo hi      : AK_CODE windows in the function body (the first one must contain [target+lo, target+hi))
#  vtslot cls k    : AK_VTSLOT; vtfunc cls k : AK_VTFUNC with prologue check (+ func witnesses); vtdata: + AK_DATA
CUI = '.?AVCUISettings@guimodule@wh@@'
IDS = [
    ('RENDERER',        0x34c6980, ('data',)),
    ('VT_UPSCALE',      0x23e9b68, ('vtslot', '.?AVCD3D9Renderer@@', 0xab8 // 8)),
    ('FN_UPSCALE',      0x029b51c, ('vtfunc', '.?AVCD3D9Renderer@@', 0xab8 // 8)),
    ('DEPTH_SRV',       0x34efab0, ('data',)),
    ('VELOCITY_TEX',    0x378f6d8, ('data',)),
    ('STEREO_EYE',      0x2ff12b4, ('data',)),
    ('JITTER_ZERO',     0x03e243f, ('patch', -8, 8)),
    ('AA_MODE',         0x358fcc4, ('data',)),
    ('PLOG',            0x29d1798, ('data',)),
    ('CONSOLE',         0x29d1768, ('data',)),
    ('POST_VIEW',       0x34b95b8, ('data',)),
    ('POST_PROJ',       0x34b95f8, ('data',)),
    ('BINDREC',         0x33ff908, ('data',)),
    ('GAMEFX',          0x3786b84, ('data',)),
    ('CUSTOMVIS',       0x3786b88, ('data',)),
    ('VELFMT_L',        0x2020671, ('patch', -20, 3)),
    ('VELFMT_R',        0x20206bc, ('patch', 0, 8)),
    ('CSYS_VT',         0x268a6a8, ('vtdata', '.?AVCSystem@@', 0)),
    ('FN_LOADXML_FILE', 0x0ea3d98, ('vtfunc', '.?AVCSystem@@', 112)),
    ('FN_LOADXML_BUF',  0x0ea3d70, ('vtfunc', '.?AVCSystem@@', 111)),
    ('PCRYPAK',         0x29d1710, ('data',)),
    # CUISettings: the settings vtable is the one of its secondary base at +0x10 (locator offset 0x10). MSVC lays it out
    # right before the primary one ([sec vt 7 slots][col ptr][primary vt]), so its slots 6/7 are primary slots -3/-2.
    ('SET_VT_LOAD',     0x22ecf80, ('vtslot', CUI, -3)),
    ('SET_VT_SAVE',     0x22ecf88, ('vtslot', CUI, -2)),
    ('FN_SET_LOAD',     0x1150d40, ('vtfunc', CUI, -3)),
    ('FN_SET_SAVE',     0x11559c0, ('vtfunc', CUI, -2)),
    ('FN_ATTR_SETI',    0x113ef10, ('func', 0, 0)),
    ('FN_ATTR_GETI',    0x113be84, ('func', 0, 0)),
    ('FN_ONACTIVATE',   0x0666a24, ('func', 0, 23)),
]
AK = {'data': 0, 'code': 1, 'vtslot': 2, 'vtfunc': 3}


class Img:
    def __init__(s, path):
        s.d = open(path, 'rb').read()
        pe = pefile.PE(data=s.d, fast_load=True)
        s.stamp, s.size, s.base = pe.FILE_HEADER.TimeDateStamp, pe.OPTIONAL_HEADER.SizeOfImage, pe.OPTIONAL_HEADER.ImageBase
        s.secs = [(x.Name.rstrip(b'\0').decode(), x.VirtualAddress, x.Misc_VirtualSize, x.PointerToRawData, x.SizeOfRawData) for x in pe.sections]
        n, v, vs, r, rs = s.sec('.text'); s.trva = v; s.text = s.d[r:r + min(vs, rs)]
        n, v, vs, r, rs = s.sec('.pdata')
        a = np.frombuffer(s.d[r:r + vs // 12 * 12], dtype='<u4').reshape(-1, 3)
        a = a[a[:, 0] != 0]; a = a[np.argsort(a[:, 0], kind='stable')]
        s.fb, s.fe, s.fu = a[:, 0].tolist(), a[:, 1].tolist(), a[:, 2].tolist()
        s.relocs = set()                      # .text bytes covered by base relocations (absolute addresses): wildcard
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
        for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in blk.entries:
                if e.type == 10 and s.trva <= e.rva < s.trva + len(s.text):
                    s.relocs.update(range(e.rva, e.rva + 8))
        s.md = Cs(CS_ARCH_X86, CS_MODE_64); s.md.detail = True
        s._dis = {}

    def sec(s, name):
        return next(x for x in s.secs if x[0] == name)

    def rd(s, rva, n):
        for name, v, vs, r, rs in s.secs:
            if v <= rva < v + max(vs, rs):
                o = rva - v
                b = s.d[r + o:r + min(o + n, rs)] if o < rs else b''
                return b + b'\0' * (n - len(b))
        raise KeyError(hex(rva))

    def q(s, rva): return struct.unpack('<Q', s.rd(rva, 8))[0]
    def dw(s, rva): return struct.unpack('<I', s.rd(rva, 4))[0]

    def frag(s, rva):          # index of the .pdata entry containing rva
        i = bisect.bisect_right(s.fb, rva) - 1
        return i if i >= 0 and rva < s.fe[i] else None

    def root(s, i):            # follow chained unwind info to the primary function fragment
        for _ in range(16):
            u = s.fu[i] & ~3
            hdr = s.rd(u, 4)
            if not (hdr[0] >> 3) & 4: return s.fb[i]
            cnt = hdr[2]; p = u + 4 + ((cnt + 1) & ~1) * 2
            b = s.dw(p); j = bisect.bisect_left(s.fb, b)
            if j >= len(s.fb) or s.fb[j] != b: return b
            i = j
        return s.fb[i]

    def dis(s, i):             # instructions of a fragment: list of (rva, size, mask bytes, is_terminator, rip_targets)
        if i in s._dis: return s._dis[i]
        b, e = s.fb[i], s.fe[i]
        out = []
        for ins in s.md.disasm(s.text[b - s.trva:e - s.trva], b):
            m = bytearray(b'\x01' * ins.size)
            rip = []; do = None
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    do = ins.disp_offset if ins.disp_size == 4 else bytes(ins.bytes).rfind(struct.pack('<i', op.mem.disp))
                    assert do > 0 and bytes(ins.bytes)[do:do + 4] == struct.pack('<i', op.mem.disp), ins
                    m[do:do + 4] = b'\0' * 4
                    rip.append(ins.address + ins.size + op.mem.disp)
            op0 = ins.bytes[0]
            if ins.imm_size == 4 and (op0 in (0xe8, 0xe9) or (op0 == 0x0f and 0x80 <= ins.bytes[1] <= 0x8f)) and ins.group(1) | ins.group(2) | ins.group(7):
                m[ins.imm_offset:ins.imm_offset + 4] = b'\0' * 4
            for k in range(ins.size):
                if ins.address + k in s.relocs: m[k] = 0
            term = ins.mnemonic in ('ret', 'retf', 'jmp', 'int3', 'ud2', 'iretq')
            out.append((ins.address, ins.size, bytes(m), term, rip, do if rip else None))
        s._dis[i] = out
        return out

    def hits(s, sig, mask, limit=2):
        """all start offsets (in .text) of sig/mask, overlap-aware, at most limit"""
        rx = re.compile(b''.join(re.escape(sig[k:k + 1]) if mask[k] else b'.' for k in range(len(sig))), re.S)
        res, pos = [], 0
        fx = [k for k in range(len(sig)) if mask[k]]
        t = s.text
        while len(res) < limit:
            m = rx.search(t, pos)
            if not m: break
            h = m.start(); res.append(h)
            for h2 in range(h + 1, min(h + len(sig), len(t) - len(sig) + 1)):   # overlapping matches inside the first
                if len(res) < limit and all(t[h2 + k] == sig[k] for k in fx): res.append(h2)
            pos = h + len(sig)
        return sorted(res)[:limit]

    def store_flag(s, i):
        """True when the fragment's root function references a string that smells of store/online code"""
        for (a, n, m, t, rip, ins) in s.dis(i):
            for r in rip:
                try: b = s.rd(r, 96)
                except KeyError: continue
                st = b.split(b'\0')[0]
                st2 = b.replace(b'\0', b'')[:48] if len(b) > 2 and b[1] == 0 else b''
                if STORE_RX.search(st) or STORE_RX.search(st2): return True
        return False

    # ---------------- MSVC RTTI ----------------
    def rtti_vtable(s, cls):
        """primary vtable rva of class cls (type descriptor by name -> COL with offset 0 -> qword at vtable[-1])"""
        name = cls.encode() + b'\0'
        tds = []
        for n, v, vs, r, rs in s.secs:
            if n not in ('.data', '.rdata'): continue
            raw = s.d[r:r + min(vs, rs)]
            for m in re.finditer(re.escape(name), raw):
                if m.start() >= 16 and (m.start() - 16) % 8 == 0: tds.append(v + m.start() - 16)
        assert len(tds) == 1, (cls, tds)
        td = tds[0]
        n, v, vs, r, rs = s.sec('.rdata'); raw = s.d[r:r + min(vs, rs)]
        cols = []
        for m in re.finditer(re.escape(struct.pack('<I', td)), raw):
            c = v + m.start() - 12
            if c % 4 == 0 and c >= v and s.dw(c) == 1 and s.dw(c + 4) == 0 and s.dw(c + 20) == c: cols.append(c)
        assert len(cols) == 1, (cls, cols)
        p = struct.pack('<Q', s.base + cols[0])
        vts = [v + m.start() + 8 for m in re.finditer(re.escape(p), raw) if m.start() % 8 == 0]
        assert len(vts) == 1, (cls, vts)
        return vts[0]


def sig_of(img, ins, lo, hi):
    a0 = ins[lo][0]; a1 = ins[hi][0] + ins[hi][1]
    sig = img.text[a0 - img.trva:a1 - img.trva]
    mask = b''.join(x[2] for x in ins[lo:hi + 1])
    return a0, sig, mask


def grow(img, ins, lo, hi, order, minfix=MINFIX, keep_lo=None):
    """extend [lo,hi] by whole instructions (order: 'fb' alternate, 'f' forward first, 'b' backward first) until unique
    with >= minfix fixed bytes and sig[0..1] fixed. Returns (a0, sig, mask) or None."""
    n = len(ins); step = 0
    while True:
        a0, sig, mask = sig_of(img, ins, lo, hi)
        nf = sum(mask)
        if len(sig) > MAXLEN or nf > MAXFIX: return None
        if nf >= minfix and mask[0] and mask[1]:
            h = img.hits(sig, mask)
            if len(h) == 1:
                assert h[0] == a0 - img.trva
                return a0, sig, mask
        can_f = hi + 1 < n and not ins[hi][3]
        can_b = lo > 0 and not ins[lo - 1][3] and (keep_lo is None or True)
        if not mask[0] or not mask[1]:          # bad start: must go back (or give up)
            if not can_b: return None
            lo -= 1; continue
        if order == 'f': pick = 'f' if can_f else 'b'
        elif order == 'b': pick = 'b' if can_b else 'f'
        else: pick = ('f' if step % 2 == 0 else 'b') if (can_f and can_b) else ('f' if can_f else 'b')
        step += 1
        if pick == 'f' and can_f: hi += 1
        elif pick == 'b' and can_b: lo -= 1
        else: return None


def find_refs(img, target):
    """.text offsets p where a rip-relative disp32 at p reaches target with tail 0,1,2 or 4"""
    if not hasattr(img, 'dsum'):   # dsum[p] = int32 at p + p  (mod 2^32), computed once
        t = np.frombuffer(img.text, dtype=np.uint8)
        n = len(t) - 3
        d = t[:n].astype(np.uint32) | (t[1:n + 1].astype(np.uint32) << 8) | (t[2:n + 2].astype(np.uint32) << 16) | (t[3:n + 3].astype(np.uint32) << 24)
        d += np.arange(n, dtype=np.uint32)
        img.dsum = d
    d = img.dsum
    out = []
    for tail in (0, 1, 2, 4):
        want = np.uint32((target - img.trva - 4 - tail) & 0xffffffff)
        out += [(int(p), tail) for p in np.nonzero(d == want)[0]]
    return sorted(out)


def data_witnesses(img, target, want=WANT, maxfrag=40):
    refs = find_refs(img, target)
    byfrag = {}
    for p, tail in refs:
        i = img.frag(img.trva + p)
        if i is not None: byfrag.setdefault(i, []).append((p, tail))
    frags = sorted(byfrag)
    if len(frags) > maxfrag:     # spread the sample over the whole section
        frags = [frags[k * len(frags) // maxfrag] for k in range(maxfrag)]
    cands = []
    for i in frags:
        ins = img.dis(i)
        idx = {x[0]: k for k, x in enumerate(ins)}
        for p, tail in byfrag[i][:3]:
            ra = img.trva + p
            k = next((k for k, x in enumerate(ins) if x[0] < ra < x[0] + x[1]), None)
            if k is None: continue
            x = ins[k]
            if not (x[5] is not None and x[0] + x[5] == ra and x[1] - x[5] - 4 == tail and target in x[4]): continue
            for order in ('fb', 'b', 'f'):
                g = grow(img, ins, k, k, order)
                if g:
                    a0, sig, mask = g
                    cands.append(dict(kind='data', a0=a0, sig=sig, mask=mask, off=ra - a0, tail=tail, root=img.root(i), frag=i))
                    break
            if len([c for c in cands if c['frag'] == i]): break
    return pick(img, cands, want), len(refs), len(byfrag)


def pick(img, cands, want):
    good = []
    seen = set()
    flags = {}
    def score(c):
        if c['frag'] not in flags: flags[c['frag']] = img.store_flag(c['frag'])
        return (flags[c['frag']], len(c['sig']) - sum(c['mask']), len(c['sig']))
    for c in sorted(cands, key=score):
        if c['root'] in seen or flags[c['frag']]: continue
        seen.add(c['root']); good.append(c)
        if len(good) == want: break
    return good


def code_witnesses(img, target, lo_b, hi_b, func, want=WANT):
    """AK_CODE windows: patch sites (all must cover [target+lo_b, target+hi_b)) or function bodies (first window covers it,
    the next ones are disjoint windows further into the body)."""
    i = img.frag(target); assert i is not None, hex(target)
    if func: assert img.fb[i] == target, 'function %x does not start a .pdata entry' % target
    ins = img.dis(i)
    starts = [x[0] for x in ins]; assert target in starts or not func
    def k_of(a):  # index of instruction containing a
        return next(k for k, x in enumerate(ins) if x[0] <= a < x[0] + x[1])
    out = []
    lo = k_of(target + lo_b); hi = k_of(target + max(hi_b, lo_b + 1) - 1)
    root = img.root(i)
    if not func:
        assert ins[lo][0] <= target + lo_b and ins[hi][0] + ins[hi][1] >= target + hi_b
        for order in ('b', 'f', 'fb'):
            g = grow(img, ins, lo, hi, order)
            if g and all(g[1] != c['sig'] for c in out):
                out.append(dict(kind='code', a0=g[0], sig=g[1], mask=g[2], off=target - g[0], root=root, frag=i))
        # a 3rd one when 'fb' equals one of the others: force a longer backward context
        if len(out) < want:
            for extra in range(1, 6):
                if lo - extra < 0 or ins[lo - extra][3]: break
                g = grow(img, ins, lo - extra, hi, 'f')
                if g and all(g[1] != c['sig'] for c in out):
                    out.append(dict(kind='code', a0=g[0], sig=g[1], mask=g[2], off=target - g[0], root=root, frag=i)); break
        for c in out:
            s0 = target + lo_b - c['a0']; s1 = target + hi_b - c['a0']
            assert 0 <= s0 and s1 <= len(c['sig'])
        return out[:want]
    k = 0
    first = True
    while len(out) < want and k < len(ins):
        if first:
            g = grow(img, ins, 0, hi, 'f')
        else:
            g = None
            for k0 in range(k, min(k + 8, len(ins))):
                if ins[k0 - 1][3] if k0 > 0 else False: break
                g = grow(img, ins, k0, k0, 'f')
                if g: break
        if not g: break
        first = False
        out.append(dict(kind='code', a0=g[0], sig=g[1], mask=g[2], off=target - g[0], root=root, frag=i))
        k = next(j for j, x in enumerate(ins) if x[0] >= g[0] + len(g[1])) if g[0] + len(g[1]) < ins[-1][0] + ins[-1][1] else len(ins)
    if out:
        assert out[0]['a0'] == target and len(out[0]['sig']) >= hi_b
    return out


def prologue(img, fn, n=24):
    i = img.frag(fn); assert i is not None and img.fb[i] == fn, hex(fn)
    ins = img.dis(i); k = 0
    while k + 1 < len(ins) and ins[k][0] + ins[k][1] - fn < n and not ins[k][3]: k += 1
    a0, sig, mask = sig_of(img, ins, 0, k)
    assert mask[0] and mask[1]
    return sig, mask


def main():
    dll, out_h = sys.argv[1:3]
    t0 = time.time()
    img = Img(dll)
    assert hashlib.sha256(img.d).hexdigest() == STEAM_SHA256, 'not the stock Steam WHGame.dll'
    ents = []   # (id, kind, off, tail, cls, sig, mask, note)
    summary = []
    for idn, (name, rva, spec) in enumerate(IDS):
        w = []; note = ''
        kind = spec[0]
        if kind in ('data', 'vtdata'):
            ds, nref, nfr = data_witnesses(img, rva)
            note = '%d refs in %d fragments' % (nref, nfr)
            for c in ds: w.append(('data', c['off'], c['tail'], None, c['sig'], c['mask'], c['a0']))
        if kind in ('vtslot', 'vtdata', 'vtfunc'):
            cls, slot = spec[1], spec[2]
            vt = img.rtti_vtable(cls)
            if kind == 'vtfunc':
                fn = img.q(vt + 8 * slot) - img.base
                assert fn == rva, (name, hex(fn))
                sig, mask = prologue(img, fn)
                w.append(('vtfunc', slot, 0, cls, sig, mask, None))
            else:
                assert vt + 8 * slot == rva, (name, hex(vt + 8 * slot))
                w.append(('vtslot', slot, 0, cls, b'', b'', None))
        if kind == 'vtfunc' or kind == 'func':
            hb = spec[2] if kind == 'func' else 0
            for c in code_witnesses(img, rva, 0, hb, True, WANT - (1 if kind == 'vtfunc' else 0)):
                w.append(('code', c['off'], 0, None, c['sig'], c['mask'], c['a0']))
        if kind == 'patch':
            for c in code_witnesses(img, rva, spec[1], spec[2], False):
                w.append(('code', c['off'], 0, None, c['sig'], c['mask'], c['a0']))
        assert w, 'no witness for ' + name
        for (k, off, tail, cls, sig, mask, a0) in w:          # proof on the Steam DLL, independent of how it was built
            if k in ('data', 'code'):
                assert mask[0] and mask[1] and len(sig) == len(mask)
                h = img.hits(sig, mask, 3); assert len(h) == 1, (name, h)
                hit = img.trva + h[0]
                tgt = hit + off + 4 + tail + struct.unpack('<i', img.text[h[0] + off:h[0] + off + 4])[0] if k == 'data' else hit + off
                assert tgt == rva, (name, k, hex(tgt))
            else:
                vt = img.rtti_vtable(cls); tgt = vt + 8 * off if k == 'vtslot' else img.q(vt + 8 * off) - img.base
                assert tgt == rva, (name, k, hex(tgt))
                if k == 'vtfunc':
                    b = img.rd(tgt, len(sig)); assert all(b[j] == sig[j] for j in range(len(sig)) if mask[j]), name
            ents.append((idn, k, off, tail, cls, sig, mask))
        desc = ', '.join('%s(%d/%d)' % (k, sum(m), len(s)) if s else k for (k, o, t, c, s, m, a) in w)
        line = '%-16s 0x%07x  %d witness(es): %s  %s' % (name, rva, len(w), desc, note)
        summary.append(line); print(line, flush=True)
    write_h(img, ents, out_h)
    js = os.path.join(os.path.dirname(os.path.abspath(out_h)), 'addr_sig.json')
    json.dump(dict(stamp=img.stamp, image=img.size, ids=[[n, r] for n, r, s in IDS],
                   ents=[dict(id=e[0], kind=e[1], off=e[2], tail=e[3], cls=e[4], sig=e[5].hex(), mask=e[6].hex()) for e in ents]),
              open(js, 'w'), indent=1)
    print('wrote %s and %s: %d ids, %d entries, %.0f s' % (out_h, js, len(IDS), len(ents), time.time() - t0))


def write_h(img, ents, out_h):
    L = ['/* generated by gen_addr.py from the stock Steam WHGame.dll (sha256 %s...): do not edit */' % STEAM_SHA256[:8],
         '#ifndef ADDR_SIG_H', '#define ADDR_SIG_H',
         'enum { ' + ', '.join('A_' + n for n, r, s in IDS) + ', A_COUNT };',
         '#define ADDR_STEAM_STAMP 0x%08xu' % img.stamp, '#define ADDR_STEAM_IMAGE 0x%08xu' % img.size,
         'static const char *const addr_name[A_COUNT] = { ' + ', '.join('"%s"' % n for n, r, s in IDS) + ' };',
         'static const unsigned int addr_steam_rva[A_COUNT] = { ' + ', '.join('0x%xu' % r for n, r, s in IDS) + ' };',
         '', 'enum { AK_DATA, AK_CODE, AK_VTSLOT, AK_VTFUNC };',
         'struct addr_ent {', '    unsigned char id;', '    unsigned char kind;', '    unsigned short len;', '    int off;', '    int tail;',
         '    const char *cls;', '    const unsigned char *sig, *mask;', '};', '']
    arr = lambda b: ','.join('0x%02x' % x for x in b)
    rows = []
    for j, (idn, k, off, tail, cls, sig, mask) in enumerate(ents):
        if sig:
            L.append('static const unsigned char addr_s%d[%d] = {%s};' % (j, len(sig), arr(sig)))
            L.append('static const unsigned char addr_m%d[%d] = {%s};' % (j, len(mask), arr(mask)))
            sm = 'addr_s%d, addr_m%d' % (j, j)
        else:
            sm = '0, 0'
        rows.append('    { A_%s, AK_%s, %d, %d, %d, %s, %s },' % (IDS[idn][0], k.upper(), len(sig), off, tail,
                                                              '"%s"' % cls if cls else '0', sm))
    L += ['', 'static const struct addr_ent addr_tab[] = {'] + rows + ['};', '#define ADDR_TAB_N (sizeof addr_tab / sizeof addr_tab[0])', '#endif', '']
    open(out_h, 'w').write('\n'.join(L))


if __name__ == '__main__':
    main()
