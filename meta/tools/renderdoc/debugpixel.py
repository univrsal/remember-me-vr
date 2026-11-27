# Debugs the pixel shader of a draw at given pixels and prints the shader's
# inputs and constant buffers (as the debugger sees them), then the final
# values of every variable it wrote. Compare two pixels to see where they part.
# Usage: rdrun.sh debugpixel.py capture.rdc <eid> <x,y> [<x,y> ...] [--consts N] [--steps]
eid = int(args[0])
pixels = []
max_consts = 32
show_steps = False
rest = args[1:]
i = 0
while i < len(rest):
    if rest[i] == "--consts":
        max_consts = int(rest[i + 1])
        i += 2
    elif rest[i] == "--steps":
        show_steps = True
        i += 1
    else:
        pixels.append(tuple(int(v) for v in rest[i].split(",")))
        i += 1
ctl.SetFrameEvent(eid, True)


def fmt(var):
    if var.members:
        return "{" + ", ".join("%s=%s" % (m.name, fmt(m)) for m in var.members) + "}"
    n = max(var.columns, 1) * max(var.rows, 1)
    if var.type in (rd.VarType.Float, rd.VarType.Half, rd.VarType.Double):
        return "(" + ", ".join("%.6g" % var.value.f32v[k] for k in range(min(n, 16))) + ")"
    return "(" + ", ".join("%d" % var.value.s32v[k] for k in range(min(n, 16))) + ")"


for x, y in pixels:
    inputs = rd.DebugPixelInputs()
    trace = ctl.DebugPixel(x, y, inputs)
    print("=== pixel (%d,%d) EID %d" % (x, y, eid))
    if trace is None or trace.debugger is None:
        print("  can't debug this pixel (not covered, or debugging unsupported)")
        continue
    for v in trace.inputs:
        print("  in  %s = %s" % (v.name, fmt(v)))
    for cb in trace.constantBlocks:
        print("  cb  %s:" % cb.name)

        def walk(var, indent):
            if var.members and len(var.members) > max_consts:
                for k, m in enumerate(var.members[:max_consts]):
                    print("%s[%d] %s = %s" % (" " * indent, k, m.name, fmt(m)))
                print("%s... (%d total)" % (" " * indent, len(var.members)))
            elif var.members and var.members[0].members:
                for m in var.members:
                    print("%s%s:" % (" " * indent, m.name))
                    walk(m, indent + 2)
            else:
                print("%s%s = %s" % (" " * indent, var.name, fmt(var)))

        for m in cb.members:
            walk(m, 4)
    final = {}
    step = 0
    while True:
        states = ctl.ContinueDebug(trace.debugger)
        if not states:
            break
        for s in states:
            for ch in s.changes:
                final[ch.after.name] = ch.after
                if show_steps:
                    print("  step %d: %s = %s" % (step, ch.after.name, fmt(ch.after)))
            step += 1
    print("  %d steps; final values:" % step)
    for name, var in final.items():
        print("    %s = %s" % (name, fmt(var)))
    ctl.FreeTrace(trace)
