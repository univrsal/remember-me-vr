# Full-frame pixel history of one or more pixels in a texture: one line per
# modification with the event, pass label, shaders and post-mod colour.
# Usage: rdrun.sh pixeltrace.py capture.rdc <resource id> <x,y> [<x,y> ...]
rid_num = int(args[0])
pixels = [tuple(int(v) for v in p.split(",")) for p in args[1:]]
tex = next(t for t in ctl.GetTextures() if int(t.resourceId) == rid_num)
sf = ctl.GetStructuredFile()
# event -> (pass label, shader label)
labels = {}
label = shaders = ""
last_eid = 0
stack = list(ctl.GetRootActions())[::-1]
while stack:
    a = stack.pop()
    stack.extend(list(a.children)[::-1])
    name = a.GetName(sf)
    if name.startswith(("Render pass", "Color pass", "Depth pass")):
        label = name.split(" (")[0]
    elif name.startswith("[vs."):
        shaders = name.strip()
    labels[a.eventId] = (label, shaders)
    last_eid = max(last_eid, a.eventId)
ctl.SetFrameEvent(last_eid, True)
sub = rd.Subresource(0, 0, 0)
for x, y in pixels:
    print("pixel (%d,%d) of %s:" % (x, y, tex.resourceId))
    for m in ctl.PixelHistory(tex.resourceId, x, y, sub, rd.CompType.Typeless):
        c = m.postMod.col.floatValue
        lab, sh = labels.get(m.eventId, ("", ""))
        flags = "".join(f for f, on in (("D", m.depthTestFailed), ("S", m.stencilTestFailed),
                                        ("X", m.scissorClipped), ("B", m.backfaceCulled),
                                        ("K", m.shaderDiscarded)) if on)
        print("  EID %5d %-15s %-28s post (%.4f %.4f %.4f %.4f) %s" % (
            m.eventId, lab, sh, c[0], c[1], c[2], c[3], flags))
