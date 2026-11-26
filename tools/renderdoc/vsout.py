# Prints the post-vertex-shader positions of draws (clip space and the
# resulting pixel position in the draw's viewport), to see where geometry lands.
# Usage: rdrun.sh vsout.py capture.rdc <eid,eid,...> [maxVerts]
import struct

eids = [int(e) for e in args[0].split(",")]
max_verts = int(args[1]) if len(args) > 1 else 8
for eid in eids:
    ctl.SetFrameEvent(eid, True)
    pipe = ctl.GetPipelineState()
    v = pipe.GetViewport(0)
    out = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    if out.vertexResourceId == rd.ResourceId.Null():
        print("EID %d: no VS output" % eid)
        continue
    data = ctl.GetBufferData(out.vertexResourceId, out.vertexByteOffset, out.vertexByteStride * out.numIndices)
    print("EID %d: %d verts, viewport x=%.1f y=%.1f w=%.1f h=%.1f" % (eid, out.numIndices, v.x, v.y, v.width, v.height))
    for i in range(min(len(data) // out.vertexByteStride, max_verts)):
        x, y, z, w = struct.unpack_from("<4f", data, i * out.vertexByteStride)
        if w == 0:
            print("   v%d clip (%.3f %.3f %.3f %.3f)" % (i, x, y, z, w))
            continue
        px = v.x + (x / w * 0.5 + 0.5) * v.width
        py = v.y + (y / w * 0.5 + 0.5) * v.height
        print("   v%d clip (%.3f %.3f %.3f %.3f) -> px (%.1f, %.1f)" % (i, x, y, z, w, px, py))
