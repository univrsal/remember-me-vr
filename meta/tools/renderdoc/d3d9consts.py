# Reads the D3D9 float constants (c0..cN) a DXVK draw used, per stage. DXVK
# passes them by buffer device address in the push data ("c" member), which
# RenderDoc's reflection doesn't follow; this decodes the address from the push
# constant bytes and reads the buffer that contains it.
# Usage: rdrun.sh d3d9consts.py capture.rdc <eid,eid,...> [count=16] [stages=vs,ps]
import struct

eids = [int(e) for e in args[0].split(",")]
count = int(args[1]) if len(args) > 1 else 16
stages = (args[2] if len(args) > 2 else "vs,ps").split(",")
smap = {"vs": rd.ShaderStage.Vertex, "ps": rd.ShaderStage.Pixel}
buffers = [b for b in ctl.GetBuffers() if b.gpuAddress]


def find_member(var_list, name):
    for v in var_list:
        if v.name == name:
            return v
    return None


for eid in eids:
    ctl.SetFrameEvent(eid, True)
    pipe = ctl.GetPipelineState()
    vk = ctl.GetVulkanPipelineState()
    push = bytes(vk.pushconsts)
    print("EID %d (push data %d bytes)" % (eid, len(push)))
    for st in stages:
        refl = pipe.GetShaderReflection(smap[st])
        if refl is None:
            continue
        block = next((cb for cb in refl.constantBlocks if cb.name == "push_data"), None)
        member = (find_member(block.variables, "c") or find_member(block.variables, "cF")) if block else None
        if member is None:
            print("  %s: no push_data.c" % st)
            continue
        off = member.byteOffset
        addr = struct.unpack_from("<Q", push, off)[0]
        buf = next((b for b in buffers if b.gpuAddress <= addr < b.gpuAddress + b.length), None)
        if buf is None:
            print("  %s: address 0x%x (push offset %d) not in any buffer" % (st, addr, off))
            continue
        data = ctl.GetBufferData(buf.resourceId, addr - buf.gpuAddress, count * 16)
        print("  %s: c[] at 0x%x (%s + 0x%x)" % (st, addr, buf.resourceId, addr - buf.gpuAddress))
        for r in range(min(count, len(data) // 16)):
            print("    c%-3d %s" % (r, " ".join("%12.6g" % v for v in struct.unpack_from("<4f", data, r * 16))))
