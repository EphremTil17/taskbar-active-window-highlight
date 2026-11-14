import struct, sys, os, urllib.request, uuid
def pdb_info(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    ddir = opt + (112 if magic == 0x20b else 96)
    dbg_rva, dbg_size = struct.unpack_from('<II', d, ddir + 6 * 8)
    secs = []
    for i in range(nsec):
        s = pe + 24 + optsz + i * 40
        vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, s + 8)
        secs.append((va, max(vsz, rsz), rptr))
    def off(rva):
        for va, sz, rp in secs:
            if va <= rva < va + sz:
                return rva - va + rp
    o = off(dbg_rva)
    for i in range(dbg_size // 28):
        typ, size, rva, ptr = struct.unpack_from('<IIII', d, o + i * 28 + 12)
        if typ == 2 and d[ptr:ptr + 4] == b'RSDS':
            g = uuid.UUID(bytes_le=d[ptr + 4:ptr + 20])
            age = struct.unpack_from('<I', d, ptr + 20)[0]
            name = d[ptr + 24:d.index(b'\0', ptr + 24)].decode()
            return os.path.basename(name), g.hex.upper() + format(age, 'X')
dll, outdir = sys.argv[1], sys.argv[2]
name, sig = pdb_info(dll)
out = os.path.join(outdir, f'{os.path.splitext(os.path.basename(dll))[0]}.pdb')
url = f'https://msdl.microsoft.com/download/symbols/{name}/{sig}/{name}'
print(url)
req = urllib.request.Request(url, headers={'User-Agent': 'Microsoft-Symbol-Server/10.0.0.0'})
with urllib.request.urlopen(req) as r, open(out, 'wb') as f:
    f.write(r.read())
print(out, os.path.getsize(out))
