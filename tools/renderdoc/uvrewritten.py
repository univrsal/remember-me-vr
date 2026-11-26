# Lists draws in a viewport rect whose input UVs (float2 at 16) look like the
# output of draw_fixes' rewrite: u = (x0 + rel_u * w) / texW, v = (y0 + rel_v * h) / texH
# (rel = position within the viewport), i.e. draws that were rewritten.
# Usage: rdrun.sh uvrewritten.py capture.rdc <x> <y> <w> <h>
import struct

rx, ry, rw, rh = (float(a) for a in args[:4])
textures = {t.resourceId: t for t in ctl.GetTextures()}
sf = ctl.GetStructuredFile()
label = shaders = ""
draws = []
stack = list(ctl.GetRootActions())[::-1]
while stack:
    a = stack.pop()
    stack.extend(list(a.children)[::-1])
    name = a.GetName(sf)
    if name.startswith(("Render pass", "Color pass")):
        label = name.split(" (")[0]
    elif name.startswith("[vs."):
        shaders = name.strip()
    elif a.flags & rd.ActionFlags.Drawcall and a.numIndices <= 64:
        draws.append((a, label, shaders))
for a, lab, sh in draws:
    ctl.SetFrameEvent(a.eventId, True)
    pipe = ctl.GetPipelineState()
    v = pipe.GetViewport(0)
    if abs((v.x - 0.5) - rx) > 0.6 or abs(v.width - rw) > 0.6 or abs(abs(v.height) - rh) > 0.6:
        continue
    rt = textures.get(pipe.GetOutputTargets()[0].resource)
    reads = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True)
    tex = textures.get(reads[0].descriptor.resource) if reads else None
    vbs = pipe.GetVBuffers()
    if not rt or not tex or not vbs or vbs[0].resourceId == rd.ResourceId.Null():
        continue
    stride = vbs[0].byteStride
    if stride < 24:
        continue
    data = ctl.GetBufferData(vbs[0].resourceId, vbs[0].byteOffset + a.vertexOffset * stride, stride * 4)
    ok = len(data) >= stride * 4
    for i in range(4 if ok else 0):
        x, y, z, w = struct.unpack_from("<4f", data, i * stride)
        u, vv = struct.unpack_from("<2f", data, i * stride + 16)
        if w == 0:
            ok = False
            break
        ru, rv = (x / w + 1) / 2, (1 - y / w) / 2
        if abs(u - (rx + ru * rw) / tex.width) > 2 / rw or abs(vv - (ry + rv * rh) / tex.height) > 2 / rh:
            ok = False
            break
    if ok:
        print("EID %d %-14s %-28s -> %s %dx%d reads %s %dx%d (stride %d)" % (
            a.eventId, lab, sh, rt.resourceId, rt.width, rt.height, tex.resourceId, tex.width, tex.height, stride))
