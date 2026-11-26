// Finds "CALL f ; MOV [reg+<offset>], EAX" sequences, i.e. a field initialised
// from a function's return value, and reports whether f (or a function it
// calls directly) calls a given allocator. Used to find e.g. the code that does
// LocalPlayer->ViewState = AllocateViewState().
//
// Script arguments: <offset hex> <allocator VA hex> [maxCallDepth=2] [window=1]
// window > 1 also reports stores of any register within <window> instructions
// after a call that reaches the allocator (catches inlined `new T()` sequences).
// Writes to the output file given as out:<path> (first argument) if present.
// @category UE3

import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;

public class FindStoreAfterCall extends GhidraScript {
  private PrintWriter out;

  private void emit(String s) {
    if (out != null) out.println(s); else println(s);
  }

  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    int i = 0;
    if (args.length > 0 && args[0].startsWith("out:")) {
      out = new PrintWriter(args[0].substring(4), "UTF-8");
      i = 1;
    }
    long offset = Long.parseLong(args[i].replace("0x", ""), 16);
    Address allocator = toAddr(args[i + 1]);
    int depth = args.length > i + 2 ? Integer.parseInt(args[i + 2]) : 2;
    int window = args.length > i + 3 ? Integer.parseInt(args[i + 3]) : 1;
    int sinceAllocCall = Integer.MAX_VALUE;
    Address lastAllocCall = null;

    int found = 0;
    InstructionIterator it = currentProgram.getListing().getInstructions(true);
    Instruction prev = null;
    while (it.hasNext() && !monitor.isCancelled()) {
      Instruction ins = it.next();
      if (ins.getFlowType().isCall()) {
        Address c = calleeOf(ins);
        if (window > 1 && c != null && callsWithin(c, allocator, depth, new HashSet<>())) {
          sinceAllocCall = 0;
          lastAllocCall = c;
        }
      } else if (sinceAllocCall < window && isStoreRegAtOffset(ins, offset)) {
        Function f = getFunctionContaining(ins.getAddress());
        emit(String.format("NEAR  %s in %s ; %d insns after call to %s", ins.getAddress(),
            f == null ? "?" : f.getName(true) + "@" + f.getEntryPoint(), sinceAllocCall, lastAllocCall));
        found++;
      }
      if (sinceAllocCall != Integer.MAX_VALUE) sinceAllocCall++;
      if (prev != null && prev.getFlowType().isCall() && isStoreEaxAtOffset(ins, offset)) {
        Address callee = calleeOf(prev);
        boolean allocates = callee != null && callsWithin(callee, allocator, depth, new HashSet<>());
        Function f = getFunctionContaining(ins.getAddress());
        Function cf = callee == null ? null : getFunctionAt(callee);
        emit(String.format("%s %s in %s ; callee %s%s", allocates ? "ALLOC" : "     ", ins.getAddress(),
            f == null ? "?" : f.getName(true) + "@" + f.getEntryPoint(),
            callee == null ? "(indirect)" : callee + (cf == null ? "" : " " + cf.getName(true) + " (" + cf.getBody().getNumAddresses() + " bytes)"),
            ""));
        found++;
      }
      prev = ins;
    }
    emit("FindStoreAfterCall: " + found + " sites");
    if (out != null) out.close();
  }

  private boolean isStoreEaxAtOffset(Instruction ins, long offset) {
    if (!ins.getMnemonicString().equalsIgnoreCase("MOV") || ins.getNumOperands() != 2) return false;
    if ((ins.getOperandType(0) & OperandType.DYNAMIC) == 0) return false;  // memory destination
    String src = ins.getDefaultOperandRepresentation(1);
    if (!src.equalsIgnoreCase("EAX")) return false;
    for (Object o : ins.getOpObjects(0)) {
      if (o instanceof Scalar s && s.getUnsignedValue() == offset) return true;
    }
    return false;
  }

  private boolean isStoreRegAtOffset(Instruction ins, long offset) {
    if (!ins.getMnemonicString().equalsIgnoreCase("MOV") || ins.getNumOperands() != 2) return false;
    if ((ins.getOperandType(0) & OperandType.DYNAMIC) == 0) return false;
    if ((ins.getOperandType(1) & OperandType.REGISTER) == 0) return false;
    for (Object o : ins.getOpObjects(0)) {
      if (o instanceof Scalar s && s.getUnsignedValue() == offset) return true;
    }
    return false;
  }

  private Address calleeOf(Instruction call) {
    for (Reference r : call.getReferencesFrom()) {
      if (r.getReferenceType().isCall()) return r.getToAddress();
    }
    return null;
  }

  private boolean callsWithin(Address fn, Address target, int depth, Set<Address> seen) {
    if (fn.equals(target)) return true;
    if (depth <= 0 || !seen.add(fn)) return false;
    Function f = getFunctionAt(fn);
    if (f == null) return false;
    InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
    while (it.hasNext()) {
      Instruction ins = it.next();
      if (!ins.getFlowType().isCall()) continue;
      Address c = calleeOf(ins);
      if (c != null && callsWithin(c, target, depth - 1, seen)) return true;
    }
    return false;
  }
}
