# Prints a draw's vertex input layout (attributes, formats, offsets, buffer
# strides) and the action's draw parameters.
# Usage: rdrun.sh vsin.py capture.rdc <eid>
eid = int(args[0])
ctl.SetFrameEvent(eid, True)
pipe = ctl.GetPipelineState()
for a in pipe.GetVertexInputs():
    print("attr %s: format %s, buffer %d, offset %d" % (a.name, a.format.Name(), a.vertexBuffer, a.byteOffset))
for i, b in enumerate(pipe.GetVBuffers()):
    if b.resourceId != rd.ResourceId.Null():
        print("vb %d: %s offset %d stride %d" % (i, b.resourceId, b.byteOffset, b.byteStride))
