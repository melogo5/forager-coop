# Decompresses every zlib stream in a Forager .cin save and reports sizes and object-name counts.
import sys, zlib, re
data = open(sys.argv[1], 'rb').read()
i, streams = 0, []
while i < len(data) - 1:
    if data[i] == 0x78 and data[i + 1] in (0x01, 0x5e, 0x9c, 0xda):
        z = zlib.decompressobj()
        try:
            out = z.decompress(data[i:])
            streams.append((i, out))
            i += len(data[i:]) - len(z.unused_data)
            continue
        except zlib.error:
            pass
    i += 1
for off, out in streams:
    names = re.findall(rb'obj[A-Z][A-Za-z]+', out)
    counts = {}
    for n in names:
        counts[n.decode()] = counts.get(n.decode(), 0) + 1
    print(f'stream @{off}: {len(out)} bytes; objects: {counts}')
    if len(sys.argv) > 2:
        print(out[:int(sys.argv[2])])
