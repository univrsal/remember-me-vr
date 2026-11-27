# Dumps the API calls (with parameters) recorded for each event in a range,
# e.g. to see clear rects and values.
# Usage: rdrun.sh apicalls.py capture.rdc <firstEID> <lastEID> [maxDepth]
first, last = int(args[0]), int(args[1])
max_depth = int(args[2]) if len(args) > 2 else 4
sf = ctl.GetStructuredFile()


def dump(obj, indent, depth):
    if depth > max_depth:
        return
    data = obj.data.basic
    if obj.NumChildren() == 0:
        val = obj.AsString() if obj.type.basetype == rd.SDBasic.String else (
            obj.AsFloat() if obj.type.basetype == rd.SDBasic.Float else (
                obj.AsInt() if obj.type.basetype in (rd.SDBasic.SignedInteger, rd.SDBasic.Enum) else (
                    obj.AsInt() if obj.type.basetype == rd.SDBasic.UnsignedInteger else (
                        obj.AsBool() if obj.type.basetype == rd.SDBasic.Boolean else (
                            obj.AsResourceId() if obj.type.basetype == rd.SDBasic.Resource else "")))))
        print("%s%s = %s" % ("  " * indent, obj.name, val))
    else:
        print("%s%s:" % ("  " * indent, obj.name))
        for i in range(obj.NumChildren()):
            dump(obj.GetChild(i), indent + 1, depth + 1)


stack = list(ctl.GetRootActions())[::-1]
while stack:
    a = stack.pop()
    if first <= a.eventId <= last:
        print("EID %d: %s" % (a.eventId, a.GetName(sf)))
        for ev in a.events:
            if first <= ev.eventId <= last:
                chunk = sf.chunks[ev.chunkIndex]
                print(" event %d chunk %s" % (ev.eventId, chunk.name))
                for i in range(chunk.NumChildren()):
                    dump(chunk.GetChild(i), 2, 1)
    stack.extend(list(a.children)[::-1])
