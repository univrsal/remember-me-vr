// Prints decompiled C for functions, or the entries of a vtable, for reading
// engine code from the terminal.
//
// Script arguments (one or more):
//   fn:<addr>              decompile the function containing <addr>
//   vt:<addr>[:<count>]    list <count> (default 64) vtable slots starting at <addr>
//   xref:<addr>            list callers / references to <addr>
//   str:<text>             find defined strings containing <text> and their referencing functions
//   out:<path>             write output to <path> instead of the Ghidra console (must come first)
// Addresses are hex VAs at the preferred base (0x00400000), as shown in Ghidra.
//
// Headless (read-only, no re-analysis):
//   analyzeHeadless re/ghidra RememberMe -process RememberMe.exe.unpacked.exe -noanalysis -readOnly \
//     -scriptPath tools/ghidra -postScript Decompile.java fn:0x00c8e0d0 vt:0x01234567:40
// @category UE3

import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class Decompile extends GhidraScript {
  private DecompInterface decompiler;
  private PrintWriter out;

  private void emit(String line) {
    if (out != null) out.println(line);
    else println(line);
  }

  @Override
  protected void run() throws Exception {
    decompiler = new DecompInterface();
    decompiler.openProgram(currentProgram);
    for (String arg : getScriptArgs()) {
      String[] parts = arg.split(":");
      if (parts[0].equals("out")) {
        out = new PrintWriter(arg.substring(4), "UTF-8");
        continue;
      }
      switch (parts[0]) {
        case "fn" -> decompile(toAddr(parts[1]));
        case "vt" -> vtable(toAddr(parts[1]), parts.length > 2 ? Integer.parseInt(parts[2]) : 64);
        case "xref" -> xrefs(toAddr(parts[1]));
        case "str" -> strings(arg.substring(4));
        default -> emit("unknown argument: " + arg);
      }
    }
    decompiler.dispose();
    if (out != null) out.close();
  }

  private String describe(Address a) {
    Function f = getFunctionContaining(a);
    return f == null ? a.toString() : a + " " + f.getName(true) + (f.getEntryPoint().equals(a) ? "" : "+" + a.subtract(f.getEntryPoint()));
  }

  private void decompile(Address at) {
    Function f = getFunctionContaining(at);
    if (f == null) {
      disassemble(at);
      f = createFunction(at, null);
    }
    if (f == null) {
      emit("=== no function at " + at);
      return;
    }
    DecompileResults r = decompiler.decompileFunction(f, 120, monitor);
    emit("=== " + f.getName(true) + " @ " + f.getEntryPoint() + " (" + f.getBody().getNumAddresses() + " bytes)");
    emit(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "decompile failed: " + r.getErrorMessage());
  }

  private void vtable(Address start, int count) throws Exception {
    emit("=== vtable @ " + start);
    for (int i = 0; i < count; i++) {
      Address slot = start.add(4L * i);
      long target = getInt(slot) & 0xffffffffL;
      Address t = toAddr(target);
      if (!currentProgram.getMemory().contains(t)) {
        emit(String.format("  [%3d] +0x%03x -> %08x (not in image; end of vtable?)", i, 4 * i, target));
        break;
      }
      Function f = getFunctionAt(t);
      if (f == null) {
        disassemble(t);
        f = createFunction(t, null);
      }
      emit(String.format("  [%3d] +0x%03x -> %s", i, 4 * i, f == null ? t.toString() : t + " " + f.getName(true)
          + " (" + f.getBody().getNumAddresses() + " bytes)"));
    }
  }

  private void xrefs(Address to) {
    emit("=== references to " + describe(to));
    for (Reference r : getReferencesTo(to)) {
      emit("  " + r.getReferenceType() + " from " + describe(r.getFromAddress()));
    }
  }

  private void strings(String needle) {
    emit("=== strings containing \"" + needle + "\"");
    DataIterator it = currentProgram.getListing().getDefinedData(true);
    while (it.hasNext() && !monitor.isCancelled()) {
      Data d = it.next();
      if (!d.hasStringValue()) continue;
      String value = StringDataInstance.getStringDataInstance(d).getStringValue();
      if (value == null || !value.contains(needle)) continue;
      emit("  " + d.getAddress() + " \"" + value + "\"");
      for (Reference r : getReferencesTo(d.getAddress())) emit("      <- " + describe(r.getFromAddress()));
    }
  }
}
