# ASCII map of a texture channel at one event on a pixel grid: each cell shows
# the mean value bucketed into 0-9 between the global min and max
# (' ' = exactly zero everywhere in the cell). Also prints per-half statistics.
# Usage: rdrun.sh valuemap.py capture.rdc <resource id> <eid> [channel] [step] [cellsX] [cellsY]
rid_num = int(args[0])
eid = int(args[1])
chan = int(args[2]) if len(args) > 2 else 0
step = int(args[3]) if len(args) > 3 else 8
cells_x = int(args[4]) if len(args) > 4 else 96
cells_y = int(args[5]) if len(args) > 5 else 40
tex = next(t for t in ctl.GetTextures() if int(t.resourceId) == rid_num)
sub = rd.Subresource(0, 0, 0)
ctl.SetFrameEvent(eid, True)
vals = {}
for y in range(step // 2, tex.height, step):
    for x in range(step // 2, tex.width, step):
        vals[(x, y)] = ctl.PickPixel(tex.resourceId, x, y, sub, rd.CompType.Typeless).floatValue[chan]
half = tex.width // 2
for name, lo, hi in (("left", 0, half), ("right", half, tex.width)):
    v = [val for (x, _), val in vals.items() if lo <= x < hi]
    nz = sum(1 for t in v if t != 0)
    print("%s: min %.6g max %.6g mean %.6g, non-zero %d/%d" % (name, min(v), max(v), sum(v) / len(v), nz, len(v)))
lo_v, hi_v = min(vals.values()), max(vals.values())
cw, ch = tex.width / cells_x, tex.height / cells_y
grid = [[[] for _ in range(cells_x)] for _ in range(cells_y)]
for (x, y), v in vals.items():
    grid[min(int(y / ch), cells_y - 1)][min(int(x / cw), cells_x - 1)].append(v)
print("%s ch%d at EID %d, range %.6g..%.6g (%dx%d px per cell; left | right):" % (
    tex.resourceId, chan, eid, lo_v, hi_v, cw, ch))
for row in grid:
    line = ""
    for c in row:
        if not c or all(t == 0 for t in c):
            line += " "
        else:
            m = sum(c) / len(c)
            line += str(min(9, int(10 * (m - lo_v) / (hi_v - lo_v)))) if hi_v > lo_v else "0"
    print(line[:cells_x // 2] + "|" + line[cells_x // 2:])
