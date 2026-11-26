# Detailed state of one draw: targets, viewport/scissor, blend, depth, every
# PS/VS resource and sampler, and the constant buffer contents per stage.
# Usage: rdrun.sh drawinfo.py capture.rdc <eid> [stages: vs,ps]
eid = int(args[0])
stages = (args[1] if len(args) > 1 else "vs,ps").split(",")
textures = {t.resourceId: t for t in ctl.GetTextures()}
ctl.SetFrameEvent(eid, True)
pipe = ctl.GetPipelineState()


def tex(rid):
    t = textures.get(rid)
    return "%s %dx%d %s" % (rid, t.width, t.height, t.format.Name()) if t else str(rid)


print("EID %d" % eid)
for i, o in enumerate(pipe.GetOutputTargets()):
    if o.resource != rd.ResourceId.Null():
        print("  RT%d %s" % (i, tex(o.resource)))
v, s = pipe.GetViewport(0), pipe.GetScissor(0)
print("  viewport x=%.1f y=%.1f w=%.1f h=%.1f scissor x=%d y=%d w=%d h=%d" % (
    v.x, v.y, v.width, v.height, s.x, s.y, s.width, s.height))
b = pipe.GetColorBlends()[0]
print("  blend %s src %s dst %s op %s mask 0x%x" % (
    b.enabled, b.colorBlend.source, b.colorBlend.destination, b.colorBlend.operation, b.writeMask))
smap = {"vs": rd.ShaderStage.Vertex, "ps": rd.ShaderStage.Pixel}
for st in stages:
    stage = smap[st]
    refl = pipe.GetShaderReflection(stage)
    if refl is None:
        continue
    print("  %s %s entry %s" % (st, pipe.GetShader(stage), refl.entryPoint))
    for used in pipe.GetReadOnlyResources(stage, True):
        print("   read slot %s: %s" % (used.access.index, tex(used.descriptor.resource)))
    for used in pipe.GetSamplers(stage, True):
        sd = used.sampler
        print("   sampler %s: filter %s addr %s/%s" % (used.access.index, sd.filter, sd.addressU, sd.addressV))
    for i, cb in enumerate(refl.constantBlocks):
        bound = pipe.GetConstantBlock(stage, i, 0)
        vars_ = ctl.GetCBufferVariableContents(pipe.GetGraphicsPipelineObject(), pipe.GetShader(stage), stage,
                                               pipe.GetShaderEntryPoint(stage), i, bound.descriptor.resource,
                                               bound.descriptor.byteOffset, bound.descriptor.byteSize)

        def show(vs, indent):
            for var in vs:
                if var.members:
                    print("%s%s:" % (" " * indent, var.name))
                    show(var.members, indent + 2)
                else:
                    n = var.columns * max(var.rows, 1)
                    vals = [var.value.f32v[k] for k in range(n)]
                    print("%s%s = %s" % (" " * indent, var.name, ", ".join("%.6g" % x for x in vals)))

        print("   cbuffer %d %s:" % (i, cb.name))
        show(vars_, 5)
