import re, sys, ctypes
dbghelp = ctypes.WinDLL('dbghelp')
und = dbghelp.UnDecorateSymbolName
und.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
buf = ctypes.create_string_buffer(8192)
pdb, out = sys.argv[1], sys.argv[2]
data = open(pdb, 'rb').read()
names = set(m.group().decode() for m in re.finditer(rb'\?[\x21-\x7e]{6,4000}', data))
with open(out, 'w', encoding='utf-8') as f:
    for n in sorted(names):
        r = und(n.encode(), buf, 8192, 0x0)  # UNDNAME_COMPLETE
        f.write((buf.value.decode(errors='replace') if r else n) + '\n')
print(out, len(names))
