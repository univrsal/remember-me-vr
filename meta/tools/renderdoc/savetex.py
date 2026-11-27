# Saves a texture at an event as PNG (or other format by extension).
# Usage: rdrun.sh savetex.py capture.rdc <resource id> <eid> <out.png> [channel: -1 = rgb, 0..3 = single, 3 also sets alpha-as-gray]
import os

rid_num = int(args[0])
eid = int(args[1])
path = os.path.abspath(args[2])
chan = int(args[3]) if len(args) > 3 else -1
tex = next(t for t in ctl.GetTextures() if int(t.resourceId) == rid_num)
ctl.SetFrameEvent(eid, True)
s = rd.TextureSave()
s.resourceId = tex.resourceId
s.destType = rd.FileType.PNG
s.mip = 0
s.alpha = rd.AlphaMapping.Discard
if chan >= 0:
    s.channelExtract = chan
res = ctl.SaveTexture(s, path)
print("saved %s (%dx%d %s) at EID %d -> %s: %s" % (tex.resourceId, tex.width, tex.height, tex.format.Name(), eid, path, res))
