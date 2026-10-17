"""Replace YYC YYVAR references in Ghidra pseudo-C with GML names.

YYC emits `YYVAR { const char* name; int id; }` in .data for every GML builtin function and
variable it touches. Ghidra shows them as DAT_<addr> (the struct) or DAT_<addr+4> (the id).
This rewrites those to `name` / `name.id` and writes yyvar_map.tsv (address, name).

Usage: python -I annotate_yyvars.py <Forager.exe> <decompiled/code dir>
"""
import re
import struct
import sys
from pathlib import Path

IDENT = re.compile(rb"[A-Za-z_][A-Za-z0-9_]{0,63}")
DAT_REF = re.compile(r"\b_?DAT_([0-9a-f]{8})\b")
UNASSIGNED_IDS = (0, 0xFFFFFFFF)
MAX_SLOT_ID = 0x10000
# Instance/global variable access by precompiled slot id:
#   (**(code **)(*self + 4))(0x5a4)  -> self->InternalGetYYVarRef(slot)
#   self[1] + 0x5a40                 -> &self->yyvars[slot] (16-byte RValues)
SLOT_REF_CALL = re.compile(r"\(\*\*\(code \*\*\)\(\*(\w+) \+ 4\)\)\(0x([0-9a-f]+)\)")
SLOT_REF_DIRECT = re.compile(r"\b(\w+)\[1\] \+ 0x([0-9a-f]+)0\b")
GLOBAL_OBJECT = "DAT_01b7ca98"
# GameMaker runtime helpers identified from call shapes (names follow the YYC runtime API).
RUNTIME_NAMES = {
    "FUN_016ab490": "YYGML_CallBuiltin",       # (self, other, &ret, argc, fn.id, args)
    "FUN_004012b0": "FREE_RValue",
    "FUN_016cad50": "Variable_GetValue",       # (inst, slot, arrayIndex, &out)
    "FUN_016caf50": "Variable_SetValue",       # (inst, slot, arrayIndex, &val)
    "FUN_016ca140": "Variable_GetBuiltin",     # (self, builtin.id, arrayIndex, &out)
}
# Other-instance access: Variable_GetValue(inst,0x5a4,...) -> Variable_GetValue(inst,inventory,...)
SLOT_REF_OTHER = re.compile(r"\b(Variable_(?:Get|Set)Value)\((\w+),0x([0-9a-f]+),")


def load_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    image_base = struct.unpack_from("<I", data, pe + 52)[0]
    sections = {}
    for i in range(count):
        o = pe + 24 + opt_size + 40 * i
        name = data[o:o + 8].rstrip(b"\0").decode()
        _, va, raw_size, raw = struct.unpack_from("<IIII", data, o + 8)
        sections[name] = (image_base + va, raw, raw_size)
    return sections


def find_yyvars(data, sections):
    """Return ({struct address: name}, {slot id: name}) from YYVAR {name, id} pairs in .data."""
    rdata_va, rdata_raw, rdata_size = sections[".rdata"]
    data_va, data_raw, data_size = sections[".data"]
    names, slots = {}, {}
    for off in range(data_raw, data_raw + data_size - 8, 4):
        ptr, val = struct.unpack_from("<II", data, off)
        if not rdata_va <= ptr < rdata_va + rdata_size:
            continue
        m = IDENT.match(data, ptr - rdata_va + rdata_raw)
        if not m or data[m.end()] != 0 or m.group().startswith(b"gml_"):
            continue
        name = m.group().decode()
        # id is 0 for builtin functions and -1 for builtin variables until the runner assigns it;
        # user variables carry a precompiled slot id
        if val in UNASSIGNED_IDS:
            names[data_va + off - data_raw] = name
        elif val < MAX_SLOT_ID:
            slots.setdefault(val, name)
    return names, slots


def annotate(text, names, slots):
    def dat(m):
        addr = int(m.group(1), 16)
        if addr in names:
            return names[addr]
        if addr - 4 in names:
            return names[addr - 4] + ".id"
        return m.group(0)

    def slot(macro):
        def repl(m):
            name = slots.get(int(m.group(2), 16))
            return f"{macro}({m.group(1)}, {name})" if name else m.group(0)
        return repl

    def other(m):
        name = slots.get(int(m.group(3), 16))
        return f"{m.group(1)}({m.group(2)},{name}," if name else m.group(0)

    text = text.replace(GLOBAL_OBJECT, "global")
    for raw, nice in RUNTIME_NAMES.items():
        text = text.replace(raw, nice)
    text = SLOT_REF_OTHER.sub(other, text)
    text = SLOT_REF_CALL.sub(slot("GETVAR"), text)
    text = SLOT_REF_DIRECT.sub(slot("VARSLOT"), text)
    return DAT_REF.sub(dat, text)


def main():
    exe, code_dir = Path(sys.argv[1]), Path(sys.argv[2])
    data = exe.read_bytes()
    names, slots = find_yyvars(data, load_sections(data))
    (code_dir / "yyvar_map.tsv").write_text(
        "".join(f"{a:08x}\t{n}\n" for a, n in sorted(names.items())), encoding="utf-8")
    (code_dir / "var_slots.tsv").write_text(
        "".join(f"{i:#x}\t{n}\n" for i, n in sorted(slots.items())), encoding="utf-8")
    changed = 0
    for path in code_dir.rglob("*.c"):
        text = path.read_text(encoding="utf-8")
        new = annotate(text, names, slots)
        if new != text:
            path.write_text(new, encoding="utf-8")
            changed += 1
    print(f"yyvars={len(names)} slots={len(slots)} files_changed={changed}")


if __name__ == "__main__":
    main()
