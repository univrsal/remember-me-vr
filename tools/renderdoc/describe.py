# Describes the actions in an event range: name, outputs/depth target with
# sizes and formats, viewport, depth/stencil state and bound PS resources.
# Usage: rdrun.sh describe.py capture.rdc <firstEID> <lastEID>
first, last = int(args[0]), int(args[1])
textures = {t.resourceId: t for t in ctl.GetTextures()}
sf = ctl.GetStructuredFile()


def tex(rid):
    t = textures.get(rid)
    if t is None:
        return str(rid)
    return "%s %dx%d %s" % (rid, t.width, t.height, t.format.Name())


actions = []
stack = list(ctl.GetRootActions())[::-1]
while stack:
    a = stack.pop()
    if first <= a.eventId <= last:
        actions.append(a)
    stack.extend(list(a.children)[::-1])

for a in actions:
    print("EID %d: %s (flags %s, %d indices/vertices)" % (a.eventId, a.GetName(sf), a.flags, a.numIndices))
    if not (a.flags & rd.ActionFlags.Drawcall):
        continue
    ctl.SetFrameEvent(a.eventId, True)
    pipe = ctl.GetPipelineState()
    for i, o in enumerate(pipe.GetOutputTargets()):
        if o.resource != rd.ResourceId.Null():
            print("  RT%d: %s" % (i, tex(o.resource)))
    d = pipe.GetDepthTarget()
    print("  depth: %s" % (tex(d.resource) if d.resource != rd.ResourceId.Null() else "none"))
    v = pipe.GetViewport(0)
    sc = pipe.GetScissor(0)
    print("  viewport: x=%.1f y=%.1f w=%.1f h=%.1f | scissor x=%d y=%d w=%d h=%d" % (
        v.x, v.y, v.width, v.height, sc.x, sc.y, sc.width, sc.height))
    vk = ctl.GetVulkanPipelineState()
    ds = vk.depthStencil
    print("  depth test %s write %s func %s | stencil %s front(func %s ref %d cmp 0x%x pass %s fail %s dfail %s)"
          % (ds.depthTestEnable, ds.depthWriteEnable, ds.depthFunction, ds.stencilTestEnable,
             ds.frontFace.function, ds.frontFace.reference, ds.frontFace.compareMask, ds.frontFace.passOperation,
             ds.frontFace.failOperation, ds.frontFace.depthFailOperation))
    for r in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel):
        for desc in r.descriptors if hasattr(r, "descriptors") else []:
            print("  PS read: %s" % tex(desc.resource))
    try:
        for used in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True):
            print("  PS read: %s" % tex(used.descriptor.resource))
    except Exception as e:
        print("  (PS reads: %s)" % e)
