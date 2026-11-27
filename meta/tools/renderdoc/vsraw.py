# Prints the raw vertex-shader output floats of a draw (all attributes), to
# read e.g. texture coordinates next to positions.
# Usage: rdrun.sh vsraw.py capture.rdc <eid> [maxVerts]
import struct

eid = int(args[0])
max_verts = int(args[1]) if len(args) > 1 else 4
ctl.SetFrameEvent(eid, True)
for stage, name in ((rd.MeshDataStage.VSIn, "VS in"), (rd.MeshDataStage.VSOut, "VS out")):
    out = ctl.GetPostVSData(0, 0, stage) if stage == rd.MeshDataStage.VSOut else None
    if out is None:
        continue
    data = ctl.GetBufferData(out.vertexResourceId, out.vertexByteOffset, 0)
    stride = out.vertexByteStride
    print("%s: stride %d" % (name, stride))
    for i in range(min(len(data) // stride, max_verts)):
        vals = struct.unpack_from("<%df" % (stride // 4), data, i * stride)
        print("  v%d " % i + " ".join("%.5f" % v for v in vals))
