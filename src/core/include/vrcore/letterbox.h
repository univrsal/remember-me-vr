#pragma once

#include <windows.h>

namespace vrcore {

// The largest rect with the aspect ratio of a `srcWidth` x `srcHeight` image
// that fits in `dstWidth` x `dstHeight`, centred (letterbox / pillarbox).
inline RECT FitRect(LONG srcWidth, LONG srcHeight, LONG dstWidth, LONG dstHeight) {
  RECT r{0, 0, dstWidth, dstHeight};
  if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0) return r;
  // Compare dstW/dstH with srcW/srcH without rounding: wider window -> pillarbox.
  const long long wide = static_cast<long long>(dstWidth) * srcHeight;
  const long long tall = static_cast<long long>(dstHeight) * srcWidth;
  if (wide > tall) {
    const LONG w = static_cast<LONG>(tall / srcHeight);
    r.left = (dstWidth - w) / 2;
    r.right = r.left + w;
  } else if (wide < tall) {
    const LONG h = static_cast<LONG>(wide / srcWidth);
    r.top = (dstHeight - h) / 2;
    r.bottom = r.top + h;
  }
  return r;
}

}  // namespace vrcore
