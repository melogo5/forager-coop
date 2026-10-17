// Renames YYC functions from a name->refVA map, then exports decompiled pseudo-C per function.
// Args: <name_map.tsv> <output dir> [only_names.txt] [timeout seconds]
// With only_names.txt, decompiles just the listed gml_* names (index.tsv is not rewritten).
// @category Forager

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Set;
import java.util.List;
import java.util.Map;

public class ExportYycFunctions extends GhidraScript {
    private static final int DEFAULT_TIMEOUT_SECONDS = 120;
    private static final int MAX_PAYLOAD_MBYTES = 1024;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Path mapPath = Paths.get(args[0]);
        Path outDir = Paths.get(args[1]);
        Set<String> only = args.length > 2
            ? new HashSet<>(Files.readAllLines(Paths.get(args[2]), StandardCharsets.UTF_8))
            : null;
        int timeoutSeconds = args.length > 3 ? Integer.parseInt(args[3]) : DEFAULT_TIMEOUT_SECONDS;

        Map<String, Function> named = renameFunctions(Files.readAllLines(mapPath, StandardCharsets.UTF_8));
        println("Renamed " + named.size() + " functions");
        if (only != null) {
            named.keySet().retainAll(only);
        }

        DecompInterface decomp = new DecompInterface();
        DecompileOptions options = new DecompileOptions();
        options.grabFromProgram(currentProgram);
        options.setMaxPayloadMBytes(MAX_PAYLOAD_MBYTES);
        decomp.setOptions(options);
        decomp.openProgram(currentProgram);
        int ok = 0;
        int failed = 0;
        StringBuilder index = new StringBuilder();
        for (Map.Entry<String, Function> e : named.entrySet()) {
            monitor.checkCancelled();
            String name = e.getKey();
            Function fn = e.getValue();
            DecompileResults res = decomp.decompileFunction(fn, timeoutSeconds, monitor);
            String body = res.decompileCompleted()
                ? res.getDecompiledFunction().getC()
                : "// decompile failed: " + res.getErrorMessage() + "\n";
            if (res.decompileCompleted()) ok++; else failed++;
            Path file = outDir.resolve(relativePath(name));
            Files.createDirectories(file.getParent());
            Files.writeString(file, "// " + name + " @ " + fn.getEntryPoint() + "\n" + body, StandardCharsets.UTF_8);
            index.append(name).append('\t').append(fn.getEntryPoint()).append('\n');
        }
        decomp.dispose();
        if (only == null) {
            Files.writeString(outDir.resolve("index.tsv"), index.toString(), StandardCharsets.UTF_8);
        }
        println("Decompiled ok=" + ok + " failed=" + failed);
    }

    private Map<String, Function> renameFunctions(List<String> lines) throws Exception {
        Map<String, Function> named = new LinkedHashMap<>();
        int missing = 0;
        for (String line : lines) {
            String[] cols = line.split("\t");
            if (cols.length < 2) continue;
            Function fn = getFunctionContaining(toAddr(cols[1].split(",")[0]));
            if (fn == null) {
                missing++;
                continue;
            }
            if (named.containsValue(fn)) continue;
            fn.setName(cols[0], SourceType.USER_DEFINED);
            named.put(cols[0], fn);
        }
        println("No containing function for " + missing + " names");
        return named;
    }

    private static String relativePath(String name) {
        if (name.startsWith("gml_Script_")) {
            return "scripts/" + name.substring("gml_Script_".length()) + ".c";
        }
        String rest = name.substring("gml_Object_".length());
        int collision = rest.indexOf("_Collision_");
        if (collision >= 0) {
            return "objects/" + rest.substring(0, collision) + "/" + rest.substring(collision + 1) + ".c";
        }
        int cut = rest.lastIndexOf('_', rest.lastIndexOf('_') - 1);
        return "objects/" + rest.substring(0, cut) + "/" + rest.substring(cut + 1) + ".c";
    }
}
