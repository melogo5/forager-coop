// For functions the decompiler cannot handle: writes an assembly listing and a summary of
// called functions and referenced strings next to the pseudo-C layout.
// Args: <only_names.txt> <output dir>
// @category Forager

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.symbol.Reference;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class ExportFunctionSummary extends GhidraScript {
    private static final int MAX_STRING_LENGTH = 256;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> names = Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8);
        Path outDir = Paths.get(args[1]);
        for (String name : names) {
            monitor.checkCancelled();
            List<Function> found = getGlobalFunctions(name);
            if (found.isEmpty()) {
                println("Not found: " + name);
                continue;
            }
            exportOne(name, found.get(0), outDir.resolve(relativeBase(name)));
        }
    }

    private void exportOne(String name, Function fn, Path base) throws Exception {
        Map<String, Integer> calls = new TreeMap<>();
        Map<String, Integer> strings = new TreeMap<>();
        StringBuilder asm = new StringBuilder("; " + name + " @ " + fn.getEntryPoint() + "\n");
        int count = 0;
        for (Instruction ins : currentProgram.getListing().getInstructions(fn.getBody(), true)) {
            count++;
            asm.append(ins.getAddress()).append("  ").append(ins).append('\n');
            for (Reference ref : ins.getReferencesFrom()) {
                Address to = ref.getToAddress();
                if (ref.getReferenceType().isCall()) {
                    Function callee = getFunctionAt(to);
                    calls.merge(callee != null ? callee.getName() : to.toString(), 1, Integer::sum);
                } else {
                    String s = readString(to);
                    if (s != null) strings.merge(s, 1, Integer::sum);
                }
            }
        }
        Files.createDirectories(base.getParent());
        Files.writeString(Paths.get(base + ".asm"), asm.toString(), StandardCharsets.UTF_8);

        StringBuilder sum = new StringBuilder();
        sum.append("// ").append(name).append(" @ ").append(fn.getEntryPoint())
            .append(" — ").append(count).append(" instructions\n");
        sum.append("// Decompiler could not handle this function; see .asm for the full listing.\n\n");
        sum.append("== Calls (callee: count) ==\n");
        calls.forEach((k, v) -> sum.append(k).append(": ").append(v).append('\n'));
        sum.append("\n== Referenced strings (string: count) ==\n");
        strings.forEach((k, v) -> sum.append('"').append(k).append("\": ").append(v).append('\n'));
        Files.writeString(Paths.get(base + ".summary.txt"), sum.toString(), StandardCharsets.UTF_8);
        println("Summarized " + name + " (" + count + " instructions)");
    }

    private String readString(Address addr) {
        Data data = getDataAt(addr);
        if (data != null && data.hasStringValue()) {
            return String.valueOf(data.getValue());
        }
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < MAX_STRING_LENGTH; i++) {
                byte b = getByte(addr.add(i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7e) return null;
                sb.append((char) b);
            }
        } catch (MemoryAccessException | RuntimeException e) {
            return null;
        }
        return sb.length() >= 2 ? sb.toString() : null;
    }

    private static String relativeBase(String name) {
        if (name.startsWith("gml_Script_")) {
            return "scripts/" + name.substring("gml_Script_".length());
        }
        String rest = name.substring("gml_Object_".length());
        int collision = rest.indexOf("_Collision_");
        if (collision >= 0) {
            return "objects/" + rest.substring(0, collision) + "/" + rest.substring(collision + 1);
        }
        int cut = rest.lastIndexOf('_', rest.lastIndexOf('_') - 1);
        return "objects/" + rest.substring(0, cut) + "/" + rest.substring(cut + 1);
    }
}
