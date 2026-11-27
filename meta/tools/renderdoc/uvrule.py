# Lists draws that games/rememberme/draw_fixes.cpp's viewport-relative UV fix
# would rewrite: viewport == the given rect, <= 64 vertices, sampler 0 texture
# the size of RT0, and input UVs (float2 at 16) equal to each vertex's position
# within the viewport (float4 at 0) within 2 pixels.
# Usage: rdrun.sh uvrule.py capture.rdc <x> <y> <w> <h> [firstEID] [lastEID]
import struct

rx, ry, rw, rh = (float(a) for a in args[:4])
first = int(args[4]) if len(args) > 4 else 0
last = int(args[5]) if len(args) > 5 else 1 << 30
textures = {t.resourceId: t for t in ctl.GetTextures()}
sf = ctl.GetStructuredFile()
draws = []
stack = list(ctl.GetRootActions())[::-1]
while stack:
    a = stack.pop()
    stack.extend(list(a.children)[::-1])
    if a.flags & rd.ActionFlags.Drawcall and first <= a.eventId <= last:
        draws.append(a)
matches = 0
for a in draws:
    if a.numIndices > 64:
        continue
    ctl.SetFrameEvent(a.eventId, True)
    pipe = ctl.GetPipelineState()
    v = pipe.GetViewport(0)
    # DXVK reports D3D9 viewports with the half-pixel offset and flipped height.
    if abs((v.x - 0.5) - rx) > 0.6 or abs(v.width - rw) > 0.6 or abs(abs(v.height) - rh) > 0.6:
        continue
    rt = textures.get(pipe.GetOutputTargets()[0].resource)
    reads = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True)
    tex = textures.get(reads[0].descriptor.resource) if reads else None
    if not rt or not tex or (tex.width, tex.height) != (rt.width, rt.height):
        continue
    vbs = pipe.GetVBuffers()
    attrs = pipe.GetVertexInputs()
    pos = next((x for x in attrs if x.byteOffset == 0 and x.vertexBuffer == 0), None)
    uv = next((x for x in attrs if x.byteOffset == 16 and x.vertexBuffer == 0), None)
    if not pos or not uv or not vbs or vbs[0].resourceId == rd.ResourceId.Null():
        continue
    stride = vbs[0].byteStride
    count = a.numIndices
    ib = pipe.GetIBuffer()
    if a.flags & rd.ActionFlags.Indexed and ib.resourceId != rd.ResourceId.Null():
        idx = ctl.GetBufferData(ib.resourceId, ib.byteOffset + a.indexOffset * ib.byteStride, a.numIndices * ib.byteStride)
        fmt = "<%d%s" % (a.numIndices, "H" if ib.byteStride == 2 else "I")
        count = max(struct.unpack_from(fmt, idx, 0)) + 1
    data = ctl.GetBufferData(vbs[0].resourceId, vbs[0].byteOffset + a.vertexOffset * stride, 0)
    ok = True
    n = 0
    for i in range(0, min(len(data) // stride, 64)):
        x, y, z, w = struct.unpack_from("<4f", data, i * stride)
        u, vv = struct.unpack_from("<2f", data, i * stride + 16)
        if w == 0:
            ok = False
            break
        if abs(u - (x / w + 1) / 2) > 2 / rw or abs(vv - (1 - y / w) / 2) > 2 / rh:
            ok = False
            break
        n += 1
        if n >= count:
            break
    if ok and n >= 3:
        matches += 1
        print("EID %d: %s -> %s %dx%d, stride %d: would be rewritten" % (
            a.eventId, a.GetName(sf), rt.resourceId, rt.width, rt.height, stride))
print("%d matching draws" % matches)
