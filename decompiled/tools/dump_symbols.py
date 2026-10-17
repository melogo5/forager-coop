"""Extract YYC symbol names and mod-API strings from Forager.exe into text maps."""
import re
import sys
from collections import defaultdict
from pathlib import Path

exe, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
strings = sorted({s.decode() for s in re.findall(rb"[\x20-\x7e]{4,}", exe.read_bytes())})

obj_re = re.compile(r"^gml_Object_(.+?)_(Collision)_([0-9a-f_]{36})$|^gml_Object_(.+)_([A-Za-z]+)_(\d+)$")
objects = defaultdict(list)
scripts, api_sigs = [], []
for s in strings:
    if m := obj_re.match(s):
        obj, event, sub = m.group(1, 2, 3) if m.group(1) else m.group(4, 5, 6)
        objects[obj].append(f"{event}_{sub}")
    elif s.startswith("gml_Script_"):
        scripts.append(s[len("gml_Script_"):])
    elif s.startswith("::"):
        api_sigs.append(s[2:])

(out / "objects_events.txt").write_text(
    "\n".join(f"{o}: {', '.join(sorted(ev))}" for o, ev in sorted(objects.items())) + "\n", encoding="utf-8")
(out / "scripts.txt").write_text("\n".join(scripts) + "\n", encoding="utf-8")
(out / "mod_api_signatures.txt").write_text("\n".join(api_sigs) + "\n", encoding="utf-8")
mod_events = [s for s in strings if re.fullmatch(r"On[A-Z][A-Za-z]+", s)]
(out / "mod_event_names.txt").write_text("\n".join(mod_events) + "\n", encoding="utf-8")
print(f"objects={len(objects)} scripts={len(scripts)} api={len(api_sigs)} events={len(mod_events)}")
