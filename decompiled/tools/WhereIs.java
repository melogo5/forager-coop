// Prints the function containing an address, its pseudo-C head and its callers.
// Args: <hex address>
// @category Forager

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.util.Set;
import java.util.TreeSet;

public class WhereIs extends GhidraScript {
    private static final int MAX_LINES = 120;
    private static final int MAX_CALLERS = 40;

    @Override
    protected void run() throws Exception {
        var addr = toAddr(getScriptArgs()[0]);
        Function fn = getFunctionContaining(addr);
        if (fn == null) {
            printerr("WHEREIS no function at " + addr);
            return;
        }
        printerr("WHEREIS " + addr + " in " + fn.getName() + " @ " + fn.getEntryPoint());
        Set<String> callers = new TreeSet<>();
        for (Reference ref : getReferencesTo(fn.getEntryPoint())) {
            Function caller = getFunctionContaining(ref.getFromAddress());
            if (caller != null) callers.add(caller.getName());
        }
        printerr("WHEREIS callers(" + callers.size() + "): "
            + String.join(" ", callers.stream().limit(MAX_CALLERS).toList()));
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        DecompileResults res = decomp.decompileFunction(fn, 120, monitor);
        if (res.decompileCompleted()) {
            String[] lines = res.getDecompiledFunction().getC().split("\n");
            for (int i = 0; i < Math.min(lines.length, MAX_LINES); i++) printerr("WHEREIS| " + lines[i]);
        }
        decomp.dispose();
    }
}
