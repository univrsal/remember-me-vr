#include "mirror.h"

#include <string>

#include "vrcore/log.h"

namespace rm::mirror {
namespace {

View g_view = View::Left;
IDirect3DSurface9* g_temp = nullptr;  // left half of the backbuffer (D3DPOOL_DEFAULT)
UINT g_tempWidth = 0, g_tempHeight = 0;
bool g_logged = false;

bool EnsureTemp(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format) {
  if (g_temp && g_tempWidth == width && g_tempHeight == height) return true;
  ReleaseDeviceResources();
  if (FAILED(device->CreateRenderTarget(width, height, format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_temp, nullptr)) ||
      !g_temp) {
    VR_LOG_ERROR("mirror: creating a %ux%u surface failed", width, height);
    g_temp = nullptr;
    return false;
  }
  g_tempWidth = width;
  g_tempHeight = height;
  return true;
}

}  // namespace

void Configure(const vrcore::Config& config) {
  g_view = config.GetString("VR", "MirrorView", "left") == "stereo" ? View::Stereo : View::Left;
}

View GetView() { return g_view; }
void SetView(View view) { g_view = view; }

void OnPresent(IDirect3DDevice9* device, bool vrStereoFrame) {
  if (!vrStereoFrame || g_view != View::Left) return;
  IDirect3DSurface9* backbuffer = nullptr;
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) || !backbuffer) return;
  D3DSURFACE_DESC desc{};
  backbuffer->GetDesc(&desc);
  const UINT half = desc.Width / 2;
  if (half > 0 && EnsureTemp(device, half, desc.Height, desc.Format)) {
    // StretchRect and ColorFill ignore the viewport, scissor and blend state,
    // so the game's state is untouched.
    const RECT left{0, 0, static_cast<LONG>(half), static_cast<LONG>(desc.Height)};
    const LONG x = static_cast<LONG>((desc.Width - half) / 2);
    const RECT centre{x, 0, x + static_cast<LONG>(half), static_cast<LONG>(desc.Height)};
    if (SUCCEEDED(device->StretchRect(backbuffer, &left, g_temp, nullptr, D3DTEXF_NONE))) {
      device->ColorFill(backbuffer, nullptr, D3DCOLOR_XRGB(0, 0, 0));
      device->StretchRect(g_temp, nullptr, backbuffer, &centre, D3DTEXF_NONE);
      if (!g_logged) {
        g_logged = true;
        VR_LOG_INFO("mirror: window shows the left eye (%ux%u, centred in %ux%u)", half, desc.Height, desc.Width,
                    desc.Height);
      }
    }
  }
  backbuffer->Release();
}

void ReleaseDeviceResources() {
  if (g_temp) g_temp->Release();
  g_temp = nullptr;
  g_tempWidth = g_tempHeight = 0;
}

}  // namespace rm::mirror
