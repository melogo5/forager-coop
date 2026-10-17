// Writes the full pseudo-C, calling convention and callers of the functions at the given addresses.
// Args: <output file> <hex address>...
// @category Forager

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.Set;
import java.util.TreeSet;

public class DecompileAt extends GhidraScript {
    private static final int TIMEOUT_SECONDS = 300;
    private static final int MAX_CALLERS = 30;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0], StandardCharsets.UTF_8)) {
            for (int i = 1; i < args.length; i++) {
                writeFunction(out, decomp, args[i]);
            }
        } finally {
            decomp.dispose();
        }
    }

    private void writeFunction(PrintWriter out, DecompInterface decomp, String address) {
        Function fn = getFunctionContaining(toAddr(address));
        if (fn == null) {
            out.println("// ==== " + address + ": no function");
            return;
        }
        Set<String> callers = new TreeSet<>();
        int refs = 0;
        for (Reference ref : getReferencesTo(fn.getEntryPoint())) {
            refs++;
            Function caller = getFunctionContaining(ref.getFromAddress());
            if (caller != null && callers.size() < MAX_CALLERS) callers.add(caller.getName());
        }
        out.println("// ==== " + fn.getName() + " @ " + fn.getEntryPoint()
                + " cc=" + fn.getCallingConventionName() + " stackPurge=" + fn.getStackPurgeSize()
                + " refs=" + refs);
        out.println("// callers: " + String.join(", ", callers));
        DecompileResults res = decomp.decompileFunction(fn, TIMEOUT_SECONDS, monitor);
        out.println(res.decompileCompleted()
                ? res.getDecompiledFunction().getC()
                : "// decompile failed: " + res.getErrorMessage());
    }
}
