# Pixel history of a texture for pixels along columns, restricted to an event
# range: per modification, shader output vs before/after and test results.
# Usage: rdrun.sh history.py capture.rdc <resource id> <firstEID> <lastEID> <x,x,...> <y0> <y1> <ystep> [summary]
rid_num = int(args[0])
first, last = int(args[1]), int(args[2])
xs = [int(x) for x in args[3].split(",")]
y0, y1, ystep = int(args[4]), int(args[5]), int(args[6])
summary = len(args) > 7
tex = next(t for t in ctl.GetTextures() if int(t.resourceId) == rid_num)
sub = rd.Subresource(0, 0, 0)
ctl.SetFrameEvent(last, True)


def depth(mod_val):
    return mod_val.depth


for x in xs:
    stats = {}
    for y in range(y0, y1, ystep):
        for m in ctl.PixelHistory(tex.resourceId, x, y, sub, rd.CompType.Typeless):
            if not (first <= m.eventId <= last):
                continue
            key = m.eventId
            s = stats.setdefault(key, [0, 0, 0])
            s[0] += 1
            s[1] += bool(m.depthTestFailed)
            s[2] += m.Passed()
            if not summary:
                print("x=%d y=%d EID %d: shader %.7f  pre %.7f  post %.7f  depthFail %s passed %s" % (
                    x, y, m.eventId, depth(m.shaderOut), depth(m.preMod), depth(m.postMod),
                    m.depthTestFailed, m.Passed()))
    for eid, (n, failed, passed) in sorted(stats.items()):
        print("x=%d EID %d: %d fragments, %d depth-test failed, %d passed" % (x, eid, n, failed, passed))
