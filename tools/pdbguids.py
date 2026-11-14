# Minimal MSF/PDB reader: find S_PUB32 symbols matching a pattern and read 16-byte GUID data from the DLL.
import struct, sys, re, uuid, ctypes
pdb_path, dll_path, pattern = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(pdb_path, 'rb').read()
bs, _, nblocks, dir_bytes, _, map_addr = struct.unpack_from('<IIIIII', d, 32)
def blocks_data(blist, size):
    return b''.join(d[b*bs:(b+1)*bs] for b in blist)[:size]
nmap = (dir_bytes + bs - 1) // bs
dir_block_list = struct.unpack_from(f'<{nmap}I', d, map_addr*bs)
dd = blocks_data(dir_block_list, dir_bytes)
nstreams = struct.unpack_from('<I', dd)[0]
sizes = struct.unpack_from(f'<{nstreams}I', dd, 4)
off = 4 + 4*nstreams; streams = []
for sz in sizes:
    if sz == 0xFFFFFFFF: sz = 0
    n = (sz + bs - 1)//bs
    streams.append((struct.unpack_from(f'<{n}I', dd, off), sz)); off += 4*n
def stream(i): return blocks_data(*streams[i])
dbi = stream(3)
sym_rec_stream = struct.unpack_from('<H', dbi, 20)[0]
recs = stream(sym_rec_stream)
und = ctypes.WinDLL('dbghelp').UnDecorateSymbolName
und.argtypes=[ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
buf = ctypes.create_string_buffer(4096)
# PE sections
p = open(dll_path, 'rb').read()
pe = struct.unpack_from('<I', p, 0x3c)[0]; nsec = struct.unpack_from('<H', p, pe+6)[0]; optsz = struct.unpack_from('<H', p, pe+20)[0]
secs = [struct.unpack_from('<IIII', p, pe+24+optsz+i*40+8) for i in range(nsec)]  # vsz, va, rsz, rptr
i = 0
while i + 4 <= len(recs):
    reclen, kind = struct.unpack_from('<HH', recs, i)
    if kind == 0x110E:
        flags, offset, seg = struct.unpack_from('<IIH', recs, i+4)
        name = recs[i+14:recs.index(b'\0', i+14)]
        und(name, buf, 4096, 0)
        full = buf.value.decode(errors='replace')
        if re.search(pattern, full) and 1 <= seg <= nsec:
            vsz, va, rsz, rptr = secs[seg-1]
            g = uuid.UUID(bytes_le=p[rptr+offset:rptr+offset+16])
            print(f'{str(g).upper()}  {full}')
    i += 2 + reclen
