# Scans the D3D9 constants of every draw whose viewport starts at a given x
# (e.g. the right eye) for values that suggest left-view screen mapping:
#   - ScreenPositionScaleBias-like (0.25, -0.499, 0.499, ~0.2501) (left bias)
#   - DNEResolution-like (2/W, -2/H, -1, 1) not corrected
#   - any of the given "suspect" numbers (e.g. left-view bias values)
# Usage: rdrun.sh scanconsts.py capture.rdc <viewportX> <firstEID> <lastEID> [regs=32]
import struct

vx = float(args[0])
first, last = int(args[1]), int(args[2])
regs = int(args[3]) if len(args) > 3 else 32
sf = ctl.GetStructuredFile()
buffers = [b for b in ctl.GetBuffers() if b.gpuAddress]
smap = {"vs": rd.ShaderStage.Vertex, "ps": rd.ShaderStage.Pixel}
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
    elif a.flags & rd.ActionFlags.Drawcall and first <= a.eventId <= last:
        draws.append((a.eventId, label, shaders))


def consts(pipe, push, stage):
    refl = pipe.GetShaderReflection(smap[stage])
    if refl is None:
        return None
    block = next((cb for cb in refl.constantBlocks if cb.name == "push_data"), None)
    member = next((v for v in block.variables if v.name == "c"), None) if block else None
    if member is None:
        return None
    addr = struct.unpack_from("<Q", push, member.byteOffset)[0]
    buf = next((b for b in buffers if b.gpuAddress <= addr < b.gpuAddress + b.length), None)
    if buf is None:
        return None
    data = ctl.GetBufferData(buf.resourceId, addr - buf.gpuAddress, regs * 16)
    return [struct.unpack_from("<4f", data, r * 16) for r in range(len(data) // 16)]


def near(a, b, tol=1e-4):
    return abs(a - b) <= tol * max(1.0, abs(b))


hits = 0
for eid, lab, sh in draws:
    ctl.SetFrameEvent(eid, True)
    pipe = ctl.GetPipelineState()
    v = pipe.GetViewport(0)
    if abs(v.x - vx) > 1.0:
        continue
    push = bytes(ctl.GetVulkanPipelineState().pushconsts)
    for stage in ("vs", "ps"):
        c = consts(pipe, push, stage)
        if not c:
            continue
        for r, (x, y, z, w) in enumerate(c):
            why = None
            if near(x, 0.25) and near(abs(y), 0.49924, 1e-3) and near(w, 0.250106, 1e-3):
                why = "left ScreenPositionScaleBias"
            elif z == -1.0 and w == 1.0 and x > 0 and y < 0 and near(x, 2.0 / v.width, 1e-3):
                why = "uncorrected DNEResolution"
            elif z == -1.0 and w == 1.0 and x > 0 and y < 0 and x < 0.01:
                why = "DNEResolution-like (2/%.0f) for viewport %.0f" % (2.0 / x, v.width)
            if why:
                hits += 1
                print("EID %5d %-15s %-28s vp %.0f..%.0f %s c%d (%.6g, %.6g, %.6g, %.6g): %s" % (
                    eid, lab, sh, v.x, v.x + v.width, stage, r, x, y, z, w, why))
print("%d hits" % hits)
