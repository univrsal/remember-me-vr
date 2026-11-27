# Per-half statistics of a texture channel at several events (non-zero count,
# min/max/mean) on a pixel grid, plus colour blend/write-mask state at draws.
# Usage: rdrun.sh halfstats.py capture.rdc <resource id> <eid,eid,...> [channel] [step]
rid_num = int(args[0])
eids = [int(e) for e in args[1].split(",")]
chan = int(args[2]) if len(args) > 2 else 0
step = int(args[3]) if len(args) > 3 else 16
tex = next(t for t in ctl.GetTextures() if int(t.resourceId) == rid_num)
sub = rd.Subresource(0, 0, 0)
half = tex.width // 2
draws = set()
stack = list(ctl.GetRootActions())
while stack:
    a = stack.pop()
    if a.flags & rd.ActionFlags.Drawcall:
        draws.add(a.eventId)
    stack.extend(a.children)
for eid in eids:
    ctl.SetFrameEvent(eid, True)
    line = "EID %d ch%d:" % (eid, chan)
    for name, lo, hi in (("L", 0, half), ("R", half, tex.width)):
        v = [ctl.PickPixel(tex.resourceId, x, y, sub, rd.CompType.Typeless).floatValue[chan]
             for y in range(step // 2, tex.height, step) for x in range(lo + step // 2, hi, step)]
        nz = sum(1 for t in v if t != 0)
        line += "  %s non-zero %5d/%5d min %-10.4g max %-10.4g mean %-10.4g" % (
            name, nz, len(v), min(v), max(v), sum(v) / len(v))
    print(line)
    if eid in draws:
        pipe = ctl.GetPipelineState()
        for i, b in enumerate(pipe.GetColorBlends()[:1]):
            print("   blend RT%d: enable %s writeMask 0x%x src %s dst %s op %s" % (
                i, b.enabled, b.writeMask, b.colorBlend.source, b.colorBlend.destination, b.colorBlend.operation))
