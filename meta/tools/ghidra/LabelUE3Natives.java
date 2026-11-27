// Labels UE3 native functions from the engine's {const char* name, void* fn}
// registration tables (e.g. "AExamplePlayerCameraexecUpdateCameraBehaviour").
// Each entry becomes a function named <Class>::exec<Function>.
//
// Headless: analyzeHeadless <proj> <name> -process <exe> -scriptPath meta/tools/ghidra -postScript LabelUE3Natives.java
// @category UE3

import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

public class LabelUE3Natives extends GhidraScript {
  // UE3 C++ class prefixes: U (UObject), A (AActor), F (structs) followed by the class name.
  private static final Pattern NATIVE = Pattern.compile("^([UAF][A-Za-z0-9_]+?)exec([A-Za-z0-9_]+)$");

  @Override
  protected void run() throws Exception {
    Memory memory = currentProgram.getMemory();
    int labeled = 0, created = 0, scanned = 0;

    for (MemoryBlock block : memory.getBlocks()) {
      if (block.isExecute() || !block.isInitialized()) continue;  // tables live in data sections
      Address cur = block.getStart();
      Address end = block.getEnd();
      while (cur.add(8).compareTo(end) <= 0 && !monitor.isCancelled()) {
        scanned++;
        Address namePtr = readPointer(memory, cur);
        Address fnPtr = readPointer(memory, cur.add(4));
        if (namePtr != null && fnPtr != null && isExecutable(memory, fnPtr)) {
          String name = readCString(memory, namePtr, 128);
          Matcher m = name == null ? null : NATIVE.matcher(name);
          if (m != null && m.matches()) {
            if (label(fnPtr, m.group(1), "exec" + m.group(2))) created++;
            labeled++;
          }
        }
        cur = cur.add(4);
      }
    }
    println(String.format("LabelUE3Natives: %d native entries labeled (%d new functions), %d slots scanned",
        labeled, created, scanned));
  }

  private boolean label(Address fn, String className, String funcName) throws Exception {
    Namespace ns = getNamespace(null, className);
    if (ns == null) ns = currentProgram.getSymbolTable().createNameSpace(null, className, SourceType.ANALYSIS);
    boolean created = false;
    Function f = getFunctionAt(fn);
    if (f == null) {
      disassemble(fn);
      f = createFunction(fn, null);
      created = f != null;
    }
    if (f != null) {
      f.setParentNamespace(ns);
      f.setName(funcName, SourceType.ANALYSIS);
    } else {
      createLabel(fn, funcName, ns, true, SourceType.ANALYSIS);
    }
    return created;
  }

  private Address readPointer(Memory memory, Address at) {
    try {
      long value = memory.getInt(at) & 0xffffffffL;
      Address target = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(value);
      return memory.contains(target) ? target : null;
    } catch (Exception e) {
      return null;
    }
  }

  private boolean isExecutable(Memory memory, Address at) {
    MemoryBlock block = memory.getBlock(at);
    return block != null && block.isExecute();
  }

  private String readCString(Memory memory, Address at, int max) {
    StringBuilder sb = new StringBuilder();
    try {
      for (int i = 0; i < max; i++) {
        byte b = memory.getByte(at.add(i));
        if (b == 0) return sb.toString();
        if (b < 0x20 || b > 0x7e) return null;
        sb.append((char) b);
      }
    } catch (MemoryAccessException e) {
      return null;
    }
    return null;
  }
}
