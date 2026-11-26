# Compares a texture before and after an event range on a pixel grid and
# prints an ASCII map of where it did NOT change (e.g. writes rejected by
# the depth test). '#' = all samples in the cell unchanged, '+' = some, '.' = none.
# Usage: rdrun.sh rejectmap.py capture.rdc <resource id> <eidBefore> <eidAfter> [step] [cellsX] [cellsY]
rid_num = int(args[0])
before, after = int(args[1]), int(args[2])
step = int(args[3]) if len(args) > 3 else 8
chan = int(args[6]) if len(args) > 6 else 0
cells_x = int(args[4]) if len(args) > 4 else 96
cells_y = int(args[5]) if len(args) > 5 else 40
tex = next(t for t in ctl.GetTextures() if int(t.resourceId) == rid_num)
sub = rd.Subresource(0, 0, 0)
xs = range(step // 2, tex.width, step)
ys = range(step // 2, tex.height, step)


def grab(eid):
    ctl.SetFrameEvent(eid, True)
    return {(x, y): ctl.PickPixel(tex.resourceId, x, y, sub, rd.CompType.Typeless).floatValue[chan]
            for y in ys for x in xs}


a, b = grab(before), grab(after)
half = tex.width // 2
for name, lo, hi in (("left", 0, half), ("right", half, tex.width)):
    keys = [k for k in a if lo <= k[0] < hi]
    same = sum(1 for k in keys if a[k] == b[k])
    print("%s half: %d/%d samples unchanged (%.1f%%)" % (name, same, len(keys), 100.0 * same / len(keys)))
cw, ch = tex.width / cells_x, tex.height / cells_y
grid = [[[0, 0] for _ in range(cells_x)] for _ in range(cells_y)]
for (x, y), v in a.items():
    c = grid[min(int(y / ch), cells_y - 1)][min(int(x / cw), cells_x - 1)]
    c[0] += 1
    c[1] += v == b[(x, y)]
print("map (%dx%d px per cell; left | right):" % (cw, ch))
for row in grid:
    line = "".join("#" if n and s == n else "+" if s else "." for n, s in row)
    print(line[:cells_x // 2] + "|" + line[cells_x // 2:])
