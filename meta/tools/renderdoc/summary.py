# Prints the capture's API, action count and render passes containing
# draws whose names match an optional filter (args[0]).
# Usage: meta/tools/renderdoc/rdrun.sh meta/tools/renderdoc/summary.py capture.rdc [filter]
flt = args[0].lower() if args else ""
props = ctl.GetAPIProperties()
print("API %s, %d textures" % (props.pipelineType, len(ctl.GetTextures())))
count = 0
stack = list(ctl.GetRootActions())[::-1]
while stack:
    a = stack.pop()
    count += 1
    name = a.GetName(ctl.GetStructuredFile())
    if flt and flt in name.lower():
        print("EID %6d  %s" % (a.eventId, name))
    stack.extend(list(a.children)[::-1])
print("%d actions" % count)
