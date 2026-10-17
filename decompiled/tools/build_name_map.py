"""Map each gml_* name string in a 32-bit YYC exe to the code addresses that reference it.

Output TSV: name<TAB>refVA[,refVA...]  (hex, absolute virtual addresses)
"""
import re
import struct
import sys
from pathlib import Path

exe, out = Path(sys.argv[1]), Path(sys.argv[2])
data = exe.read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
num_sections = struct.unpack_from("<H", data, pe + 6)[0]
opt_size = struct.unpack_from("<H", data, pe + 20)[0]
image_base = struct.unpack_from("<I", data, pe + 52)[0]

sections = {}
for i in range(num_sections):
    o = pe + 24 + opt_size + 40 * i
    name = data[o:o + 8].rstrip(b"\0").decode()
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
    sections[name] = (va, raw, rsize)


def off_to_va(off):
    for va, raw, rsize in sections.values():
        if raw <= off < raw + rsize:
            return image_base + va + off - raw
    return None


text_va, text_raw, text_size = sections[".text"]
text = data[text_raw:text_raw + text_size]

rows = []
for m in re.finditer(rb"gml_(?:Object|Script)_[\x21-\x7e]+?\x00", data):
    name = m.group()[:-1].decode()
    needle = struct.pack("<I", off_to_va(m.start()))
    refs, pos = [], text.find(needle)
    while pos != -1:
        refs.append(image_base + text_va + pos)
        pos = text.find(needle, pos + 1)
    if refs:
        rows.append(f"{name}\t{','.join(hex(r) for r in refs)}")

out.write_text("\n".join(rows) + "\n", encoding="utf-8")
print(f"mapped {len(rows)} names")
