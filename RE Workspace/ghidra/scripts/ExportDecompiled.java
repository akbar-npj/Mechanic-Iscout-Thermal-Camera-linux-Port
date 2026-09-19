// Exports decompiled C for every function in the current program.
// Usage: analyzeHeadless <proj> <name> -import <bin> -postScript ExportDecompiled.java <outFile>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.PrintWriter;
import java.io.File;

public class ExportDecompiled extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("ERROR: need output file path argument");
            return;
        }
        File out = new File(args[0]);
        PrintWriter pw = new PrintWriter(out, "UTF-8");

        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        ifc.setSimplificationStyle("decompile");

        pw.println("// Program: " + currentProgram.getName());
        pw.println("// Language: " + currentProgram.getLanguageID());
        pw.println("// ImageBase: " + currentProgram.getImageBase());
        pw.println("// Functions: " + currentProgram.getFunctionManager().getFunctionCount());
        pw.println();

        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext()) {
            Function f = it.next();
            pw.println("/* ============================================================");
            pw.println(" * " + f.getName() + "  @ " + f.getEntryPoint());
            pw.println(" * Signature: " + f.getPrototypeString(false, false));
            pw.println(" * ============================================================ */");
            try {
                DecompileResults res = ifc.decompileFunction(f, 90, monitor);
                if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                    pw.println(res.getDecompiledFunction().getC());
                } else {
                    pw.println("// [decompile failed: " + (res == null ? "null" : res.getErrorMessage()) + "]");
                }
            } catch (Exception e) {
                pw.println("// [exception: " + e.getMessage() + "]");
            }
            pw.println();
            n++;
            if (n % 25 == 0) {
                println("  decompiled " + n + " functions...");
            }
        }
        pw.flush();
        pw.close();
        ifc.dispose();
        println("DONE: wrote " + n + " functions to " + out.getAbsolutePath());
    }
}
