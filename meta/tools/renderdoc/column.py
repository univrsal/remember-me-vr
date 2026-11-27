# Samples a texture along vertical columns at given events and prints the
# values, collapsing runs of equal rows, to spot horizontal bands.
# Usage: rdrun.sh column.py capture.rdc <resource id> <eid,eid,...> <x,x,...> [ystep] [channel]
rid_num = int(args[0])
eids = [int(e) for e in args[1].split(",")]
xs = [int(x) for x in args[2].split(",")]
ystep = int(args[3]) if len(args) > 3 else 4
chan = int(args[4]) if len(args) > 4 else 0
tex = next(t for t in ctl.GetTextures() if int(t.resourceId) == rid_num)
sub = rd.Subresource(0, 0, 0)
print("%s %dx%d %s, channel %d" % (tex.resourceId, tex.width, tex.height, tex.format.Name(), chan))
for eid in eids:
    ctl.SetFrameEvent(eid, True)
    for x in xs:
        print("EID %d x=%d:" % (eid, x))
        prev, start = None, 0
        rows = list(range(0, tex.height, ystep)) + [tex.height - 1]
        for y in rows:
            v = ctl.PickPixel(tex.resourceId, x, y, sub, rd.CompType.Typeless).floatValue[chan]
            v = round(v, 6)
            if v != prev:
                if prev is not None:
                    print("   y %4d..%4d  %s" % (start, y - 1, prev))
                prev, start = v, y
        print("   y %4d..%4d  %s" % (start, tex.height - 1, prev))
