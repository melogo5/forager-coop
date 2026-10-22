// Decompiles the functions the bulk export gave up on (timeout 300 s): one at a time, with a long timeout and a big
// result payload. Each goes to <out dir>/<name>.c (an error note if it fails again).
// Args: <out dir> <timeout seconds> <payload MB, 0 = Ghidra's default> <name>:<hex address>...
// @category Forager

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class DecompileHard extends GhidraScript {
    private int payloadMb;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        int timeout = Integer.parseInt(args[1]);
        payloadMb = Integer.parseInt(args[2]);
        outDir.mkdirs();
        for (int i = 3; i < args.length; i++) {
            String[] pair = args[i].split(":");  // not "=": a .bat splits arguments at it
            decompile(new File(outDir, pair[0] + ".c"), pair[1], timeout);
        }
    }

    private void decompile(File file, String address, int timeout) throws Exception {
        Function fn = getFunctionContaining(toAddr(address));
        DecompInterface decomp = new DecompInterface();  // a fresh decompiler process per function
        DecompileOptions options = new DecompileOptions();
        if (payloadMb > 0) options.setMaxPayloadMBytes(payloadMb);
        decomp.setOptions(options);
        if (!decomp.openProgram(currentProgram)) println("openProgram failed: " + decomp.getLastMessage());
        long started = System.currentTimeMillis();
        try (PrintWriter out = new PrintWriter(file, StandardCharsets.UTF_8)) {
            if (fn == null) {
                out.println("// " + address + ": no function");
                return;
            }
            out.println("// " + fn.getName() + " @ " + fn.getEntryPoint() + " (DecompileHard, timeout " + timeout + " s)");
            DecompileResults res = decomp.decompileFunction(fn, timeout, monitor);
            out.println(res.decompileCompleted()
                    ? res.getDecompiledFunction().getC()
                    : "// decompile failed: " + res.getErrorMessage() + " / " + decomp.getLastMessage());
            println(file.getName() + ": " + (res.decompileCompleted() ? "done" : "FAILED " + res.getErrorMessage())
                    + " in " + (System.currentTimeMillis() - started) / 1000 + " s");
        } finally {
            decomp.dispose();
        }
    }
}
