# Repairs a Forager zone save (Zones/*.cin) that LoadZone cannot read: drops the empty records in its lists
# (Herbs, Instances, ...): asset_get_index(undefined) in LoadZone - an instance SaveGame listed but could not reach
# (gone, or asleep and not in global.deactivatedInstances; playtest 4, bug 10).
# Usage: python cinder_fix.py <in.cin> [out.cin]  - without out.cin it only reports.
import struct
import sys
import zlib

KEY_TABLE, FIXED = 0, {1: 1, 2: 1, 3: 2, 4: 2, 5: 4, 6: 4, 7: 2, 8: 4, 9: 8, 10: 1, 12: 8}


def read_chunks(data):
    if data[:4] != b'cndr':
        raise ValueError('not a cinder file')
    count = struct.unpack_from('<H', data, 6)[0]
    chunks = []
    for i in range(count):
        tag = data[10 + 12 * i:14 + 12 * i]
        size, offset = struct.unpack_from('<II', data, 14 + 12 * i)
        chunks.append([tag, zlib.decompress(data[offset:offset + size])])
    return chunks


def write_chunks(header, chunks):
    packed = [(tag, zlib.compress(raw)) for tag, raw in chunks]
    offset = 10 + 12 * len(packed)
    out = bytearray(header[:10])
    for tag, comp in packed:
        out += tag + struct.pack('<II', len(comp), offset)
        offset += len(comp)
    for _, comp in packed:
        out += comp
    return bytes(out)


def parse_keys(raw):
    count, p, keys = struct.unpack_from('<I', raw, 0)[0], 4, []
    for _ in range(count):
        end = raw.index(b'\0', p)
        keys.append((raw[p:end].decode(), raw[end + 1]))
        p = end + 2
    return keys


class Reader:
    def __init__(self, raw, keys):
        self.raw, self.keys, self.p = raw, keys, 0

    def skip_value(self, t):
        if t in FIXED:
            self.p += FIXED[t]
        elif t in (11, 18):
            self.p = self.raw.index(b'\0', self.p) + 1
        elif t == 16:
            element = self.raw[self.p]
            count = struct.unpack_from('<H', self.raw, self.p + 1)[0]
            self.p += 3
            for _ in range(count):
                self.skip_value(element)
        elif t == 17:
            count = struct.unpack_from('<H', self.raw, self.p)[0]
            self.p += 2
            for _ in range(count):
                self.skip_map()
        elif t == 19:
            self.p += 2 + struct.unpack_from('<H', self.raw, self.p)[0]
        else:
            raise ValueError(f'unknown type {t} at {self.p}')

    def skip_map(self):
        fields = self.raw[self.p]
        self.p += 1
        for _ in range(fields):
            key = struct.unpack_from('<H', self.raw, self.p)[0]
            self.p += 2
            self.skip_value(self.keys[key][1])


def drop_empty_records(raw, keys):
    """The zone record with the empty maps of its lists (Instances, Herbs, ...) removed, and how many there were."""
    r = Reader(raw, keys)
    out, empty = bytearray(), 0
    while r.p < len(raw):
        start = r.p
        key = struct.unpack_from('<H', raw, r.p)[0]
        t = keys[key][1]
        r.p += 2
        if t != 17:
            r.skip_value(t)
            out += raw[start:r.p]
            continue
        count = struct.unpack_from('<H', raw, r.p)[0]
        r.p += 2
        kept = []
        for _ in range(count):
            at = r.p
            r.skip_map()
            if raw[at] == 0:
                empty += 1
            else:
                kept.append(raw[at:r.p])
        out += raw[start:start + 2] + struct.pack('<H', len(kept)) + b''.join(kept)
    return bytes(out), empty


def main():
    data = open(sys.argv[1], 'rb').read()
    chunks = read_chunks(data)
    keys = parse_keys(chunks[KEY_TABLE][1])
    total = 0
    for chunk in chunks[1:]:
        chunk[1], empty = drop_empty_records(chunk[1], keys)
        total += empty
    print(f'{sys.argv[1]}: {total} empty records')
    if len(sys.argv) > 2 and total > 0:
        open(sys.argv[2], 'wb').write(write_chunks(data, chunks))
        print(f'written {sys.argv[2]}')


if __name__ == '__main__':
    main()
