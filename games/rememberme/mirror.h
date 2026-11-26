#pragma once

#include <d3d9.h>

#include "vrcore/config.h"

namespace rm::mirror {

// What the game window shows while VR runs in stereo ([VR] MirrorView):
//  - left (default): the left eye with its UI, centred in the window
//    (pillarboxed: the eye is narrower than the window).
//  - stereo: both eyes side by side, as rendered (UI over the left one).
// Mono frames (menus without a 3D view, loading screens, videos) and the flat
// F10 test are left as they are.
enum class View { Left, Stereo };

void Configure(const vrcore::Config& config);
View GetView();

// Presenting thread, before Present: rearranges the backbuffer for a stereo
// frame shown in VR.
void OnPresent(IDirect3DDevice9* device, bool vrStereoFrame);

// Before a device Reset (D3DPOOL_DEFAULT resources must be released).
void ReleaseDeviceResources();

}  // namespace rm::mirror
