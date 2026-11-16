#!/usr/bin/env python3
"""Resolve every id of addr_sig.h on any WHGame.dll, the way ADDR_CONTRACT.md says the plugin does it at run time.
usage: addr_check.py addr_sig.h|addr_sig.json WHGAME_DLL [--steam]
Independent of gen_addr.py: own PE parsing (struct only), own scanner (anchor + bytes.find), own RTTI walk, own voting.
Prints one line per id: name, resolved RVA (or UNRESOLVED), votes, witnesses (kind:fixed/len and its outcome).
--steam: additionally require every id to resolve to addr_steam_rva (exit 1 otherwise)."""
import sys, re, json, struct

AKN = ['AK_DATA', 'AK_CODE', 'AK_VTSLOT', 'AK_VTFUNC']


def load_table(path):
    if path.endswith('.json'):
        j = json.load(open(path))
        kinds = {'data': 0, 'code': 1, 'vtslot': 2, 'vtfunc': 3}
        names = [n for n, r in j['ids']]; steam = [r for n, r in j['ids']]
        ents = [(e['id'], kinds[e['kind']], e['off'], e['tail'], e['cls'], bytes.fromhex(e['sig']), bytes.fromhex(e['mask'])) for e in j['ents']]
        return names, steam, ents
    h = open(path).read()
    names = re.search(r'enum \{ (A_[^}]*), A_COUNT \}', h).group(1).replace(' ', '').split(',')
    names = [x[2:] for x in names]
    steam = [int(x.rstrip('u'), 16) for x in re.search(r'addr_steam_rva\[A_COUNT\] = \{ ([^}]*) \}', h).group(1).split(', ')]
    arrs = {m.group(1): bytes(int(x, 16) for x in m.group(2).split(',')) for m in re.finditer(r'static const unsigned char (addr_[sm]\d+)\[\d+\] = \{([^}]*)\};', h)}
    ents = []
    tab = h[h.index('addr_tab[] = {'):]
    for m in re.finditer(r'\{ A_(\w+), (AK_\w+), (\d+), (-?\d+), (-?\d+), ("[^"]*"|0), (\w+), (\w+) \},', tab):
        n, k, ln, off, tail, cls, s, mk = m.groups()
        sig = arrs.get(s, b''); mask = arrs.get(mk, b'')
        assert len(sig) == len(mask) == int(ln), m.group(0)
        ents.append((names.index(n), AKN.index(k), int(off), int(tail), None if cls == '0' else cls.strip('"'), sig, mask))
    return names, steam, ents


class Pe:
    def __init__(s, path):
        s.d = open(path, 'rb').read()
        e = struct.unpack_from('<I', s.d, 0x3c)[0]
        assert s.d[e:e + 4] == b'PE\0\0'
        nsec, = struct.unpack_from('<H', s.d, e + 6); optsz, = struct.unpack_from('<H', s.d, e + 20)
        s.stamp, = struct.unpack_from('<I', s.d, e + 8)
        o = e + 24
        assert struct.unpack_from('<H', s.d, o)[0] == 0x20b
        s.base, = struct.unpack_from('<Q', s.d, o + 24); s.size, = struct.unpack_from('<I', s.d, o + 56)
        s.secs = []
        for i in range(nsec):
            p = o + optsz + 40 * i
            name = s.d[p:p + 8].rstrip(b'\0').decode()
            vs, va, rs, rp, _, _, _, _, ch = struct.unpack_from('<IIIIIIHHI', s.d, p + 8)
            s.secs.append((name, va, vs, rp, rs, ch))
        # the code section = the executable one (contract: "WHGame.dll's code section")
        code = [x for x in s.secs if x[5] & 0x20000000]
        assert len(code) == 1, code
        n, s.cva, vs, rp, rs, ch = code[0]
        s.code = s.d[rp:rp + min(vs, rs)]
        # a flat image (as mapped), so RVAs can be dereferenced like in the process
        s.img = bytearray(s.size)
        s.img[:0x1000] = s.d[:0x1000]
        for n, va, vs, rp, rs, ch in s.secs:
            s.img[va:va + min(vs, rs)] = s.d[rp:rp + min(vs, rs)]

    def q(s, rva): return struct.unpack_from('<Q', s.img, rva)[0] if 0 <= rva <= s.size - 8 else None

    def scan(s, sig, mask):
        """all offsets in the code section where sig/mask matches (overlapping allowed)"""
        best, bl, i = (0, 0), 0, 0
        while i < len(sig):          # longest fixed run as the anchor
            if mask[i]:
                j = i
                while j < len(sig) and mask[j]: j += 1
                if j - i > bl: best, bl = (i, j), j - i
                i = j
            else: i += 1
        a0, a1 = best; anc = sig[a0:a1]; fx = [k for k in range(len(sig)) if mask[k]]
        out, p, t = [], 0, s.code
        while True:
            p = t.find(anc, p)
            if p < 0: break
            st = p - a0
            if 0 <= st <= len(t) - len(sig) and all(t[st + k] == sig[k] for k in fx): out.append(st)
            p += 1
        return out

    def vtable(s, cls):
        """primary vtable via RTTI: type descriptor named cls -> complete object locator with offset 0 -> vtable[-1]"""
        name = cls.encode() + b'\0'
        tds = [m.start() - 16 for m in re.finditer(re.escape(name), s.img) if m.start() >= 16 and (m.start() - 16) % 8 == 0]
        if len(tds) != 1: return None, 'type descriptor x%d' % len(tds)
        td = tds[0]
        cols = []
        for m in re.finditer(re.escape(struct.pack('<I', td)), s.img):
            c = m.start() - 12
            if c >= 0 and c % 4 == 0 and struct.unpack_from('<IIIIII', s.img, c)[0:2] == (1, 0) and struct.unpack_from('<I', s.img, c + 20)[0] == c:
                cols.append(c)
        if len(cols) != 1: return None, 'offset-0 locator x%d' % len(cols)
        vts = [m.start() + 8 for m in re.finditer(re.escape(struct.pack('<Q', s.base + cols[0])), s.img) if m.start() % 8 == 0]
        if len(vts) != 1: return None, 'vtable x%d' % len(vts)
        return vts[0], ''


def resolve(pe, names, ents):
    votes = {i: [] for i in range(len(names))}
    notes = {i: [] for i in range(len(names))}
    vtc = {}
    for (i, k, off, tail, cls, sig, mask) in ents:
        tgt, why = None, ''
        if k in (0, 1):
            h = pe.scan(sig, mask)
            if len(h) == 1:
                hit = pe.cva + h[0]
                tgt = hit + off + 4 + tail + struct.unpack_from('<i', pe.code, h[0] + off)[0] if k == 0 else hit + off
            else: why = '%d hits' % len(h)
            desc = '%s:%d/%d' % (AKN[k][3:].lower(), sum(mask), len(sig))
        else:
            if cls not in vtc: vtc[cls] = pe.vtable(cls)
            vt, why = vtc[cls]
            if vt is not None:
                if k == 2: tgt = vt + 8 * off
                else:
                    v = pe.q(vt + 8 * off)
                    tgt = v - pe.base if v is not None and pe.base <= v < pe.base + pe.size else None
                    if tgt is None: why = 'slot outside image'
                    elif sig and not all(pe.img[tgt + j] == sig[j] for j in range(len(sig)) if mask[j]):
                        tgt, why = None, 'prologue differs'
            desc = '%s:%s[%d]%s' % (AKN[k][3:].lower(), cls, off, '+%dB' % sum(mask) if sig else '')
        if tgt is not None: votes[i].append(tgt)
        notes[i].append(desc + ('=%x' % tgt if tgt is not None else ' (' + why + ')'))
    res = {}
    for i in votes:
        v = set(votes[i])
        res[i] = v.pop() if len(v) == 1 else None
    return res, votes, notes


def main():
    tabp, dll = sys.argv[1:3]
    steam_mode = '--steam' in sys.argv[3:]
    names, steam, ents = load_table(tabp)
    pe = Pe(dll)
    print('%s: stamp 0x%08x image 0x%x, code section 0x%x+0x%x, %d ids, %d entries' % (dll, pe.stamp, pe.size, pe.cva, len(pe.code), len(names), len(ents)))
    res, votes, notes = resolve(pe, names, ents)
    bad = 0
    for i, n in enumerate(names):
        r = res[i]
        ok = r is not None and (not steam_mode or r == steam[i])
        bad += not ok
        print('%-16s %-10s votes %d/%d  steam 0x%07x%s  %s' % (n, '0x%07x' % r if r is not None else 'UNRESOLVED', len(votes[i]), len(notes[i]),
              steam[i], ' (+0x%x)' % (r - steam[i]) if r is not None and r != steam[i] else '', ' '.join(notes[i])))
    print('%d of %d ids resolved%s' % (len(names) - bad, len(names), ' to the Steam RVAs' if steam_mode else ''))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
