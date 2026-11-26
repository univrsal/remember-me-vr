# One line per draw in an event range: pass label, shaders label, target,
# viewport, scissor. Usage: rdrun.sh passes.py capture.rdc <firstEID> <lastEID>
first, last = int(args[0]), int(args[1])
textures = {t.resourceId: t for t in ctl.GetTextures()}
sf = ctl.GetStructuredFile()
label = shaders = ""
stack = list(ctl.GetRootActions())[::-1]
while stack:
    a = stack.pop()
    stack.extend(list(a.children)[::-1])
    if not (first <= a.eventId <= last):
        continue
    name = a.GetName(sf)
    if name.startswith(("Render pass", "Color pass", "Depth pass")):
        label = name.split(" (")[0]
    elif name.startswith("[vs."):
        shaders = name.strip()
    elif a.flags & rd.ActionFlags.Drawcall:
        ctl.SetFrameEvent(a.eventId, True)
        pipe = ctl.GetPipelineState()
        rt = pipe.GetOutputTargets()[0].resource
        t = textures.get(rt)
        v, s = pipe.GetViewport(0), pipe.GetScissor(0)
        print("EID %5d %-15s %-28s %5d idx -> %s %s | vp x=%-6.1f w=%-6.1f | sc x=%-4d w=%-4d" % (
            a.eventId, label, shaders, a.numIndices, rt, "%dx%d" % (t.width, t.height) if t else "",
            v.x, v.width, s.x, s.width))
