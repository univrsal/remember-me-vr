# Lists every event that uses a resource, with the usage kind.
# Usage: rdrun.sh usage.py capture.rdc <resource id number> [firstEID] [lastEID]
rid_num = int(args[0])
first = int(args[1]) if len(args) > 1 else 0
last = int(args[2]) if len(args) > 2 else 1 << 30
target = None
for t in ctl.GetTextures():
    if int(t.resourceId) == rid_num:
        target = t
        break
if target is None:
    raise SystemExit(print("no texture %d" % rid_num))
print("%s %dx%d %s" % (target.resourceId, target.width, target.height, target.format.Name()))
for u in ctl.GetUsage(target.resourceId):
    if first <= u.eventId <= last:
        print("  EID %6d  %s" % (u.eventId, u.usage))
