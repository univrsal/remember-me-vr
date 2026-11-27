#include "render_size.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include "engine.h"
#include "vrcore/hook.h"
#include "vrcore/letterbox.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"
#include "vrcore/xr.h"

namespace rm::render_size {
namespace {

enum class Mode { Window, Auto, Fixed };

Mode g_mode = Mode::Window;
float g_scale = 1.0f;
uint32_t g_fixedWidth = 0, g_fixedHeight = 0;
bool g_enabled = false;
bool g_installed = false;

// Headset eye size (auto): from the startup probe, or later the compositor.
std::atomic<uint32_t> g_eyeWidth{0}, g_eyeHeight{0};
// What the last UpdateViewportRHI got from us (the backbuffer size); 0 = not
// overridden. Game thread writes; mouse hooks (game thread) and Managed() read.
std::atomic<uint32_t> g_outWidth{0}, g_outHeight{0};
std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_resizePosted{false};

bool g_vr = false;
std::atomic<bool> g_forceWindowed{true};
std::atomic<bool> g_disableVSync{true};

// The window settings as the game loaded them and as the mod replaced them
// (game thread: loaded once in UGameEngine::Init, saved by the game menu).
struct WindowPatch {
  bool active = false;
  int32_t fullscreen = 0, width = 0, height = 0;                  // the mod's
  int32_t origFullscreen = 0, origWidth = 0, origHeight = 0;     // the player's
};
WindowPatch g_patch;

UpdateViewportRhiFn g_origUpdate = nullptr;
GfxSettingsFn g_origGfxLoad = nullptr;
GfxSettingsFn g_origGfxSave = nullptr;
ViewportMouseAxisFn g_origMouseX = nullptr;
ViewportMouseAxisFn g_origMouseY = nullptr;
ViewportMousePosFn g_origMousePos = nullptr;
ViewportSetMouseFn g_origSetMouse = nullptr;

// Total render size (both eyes side by side), or false if not known (yet).
bool Target(uint32_t& width, uint32_t& height) {
  switch (g_mode) {
    case Mode::Fixed:
      width = g_fixedWidth;
      height = g_fixedHeight;
      return true;
    case Mode::Auto: {
      const uint32_t ew = g_eyeWidth, eh = g_eyeHeight;
      if (!ew || !eh) return false;
      width = 2 * static_cast<uint32_t>(std::lround(static_cast<float>(ew) * g_scale));
      height = static_cast<uint32_t>(std::lround(static_cast<float>(eh) * g_scale));
      return width > 0 && height > 0;
    }
    case Mode::Window:
    default:
      return false;
  }
}

// GSystemSettings-like gfx config's SuperSampleCount (what UpdateViewportRHI multiplies by).
uint32_t SuperSampleCount() {
  uintptr_t settings = 0;
  int32_t count = 1;
  if (!vrcore::mem::Read(reinterpret_cast<uintptr_t>(vrcore::Exe().FromPreferredVA(kGfxSettings)), settings) ||
      !settings || !vrcore::mem::Read(settings + kGfxSettingsSuperSampleCount, count)) {
    return 1;
  }
  return static_cast<uint32_t>(std::clamp(count, 1, 8));
}

HWND WindowOf(void* viewport) {
  HWND window = nullptr;
  vrcore::mem::Read(reinterpret_cast<uintptr_t>(viewport) + kViewportWindow, window);
  return window;
}

void __fastcall HookUpdateViewportRhi(void* viewport, void* /*edx*/, int destroyed, uint32_t sizeX, uint32_t sizeY,
                                      int fullscreen) {
  uint32_t width = 0, height = 0;
  if (destroyed) {
    g_outWidth = g_outHeight = 0;
  } else {
    if (HWND window = WindowOf(viewport)) g_window = window;
    const uint32_t windowX = sizeX, windowY = sizeY;
    if (!fullscreen && Target(width, height)) {
      // The engine multiplies by SuperSampleCount itself; ask for at least `width` x `height`.
      const uint32_t ss = SuperSampleCount();
      sizeX = (width + ss - 1) / ss;
      sizeY = (height + ss - 1) / ss;
      const bool changed = sizeX != g_outWidth || sizeY != g_outHeight;
      g_outWidth = sizeX;
      g_outHeight = sizeY;
      if (changed) {
        VR_LOG_INFO("render size: %ux%u (both eyes) instead of the %ux%u window; engine viewport %ux%u x "
                    "SuperSampleCount %u, backbuffer %ux%u scaled into the window",
                    sizeX * ss, sizeY * ss, windowX, windowY, sizeX, sizeY, ss, sizeX, sizeY);
      }
    } else {
      if (g_outWidth) {
        VR_LOG_INFO("render size: back to the window (%ux%u%s)", sizeX, sizeY, fullscreen ? ", fullscreen" : "");
      }
      g_outWidth = g_outHeight = 0;
    }
  }
  g_origUpdate(viewport, destroyed, sizeX, sizeY, fullscreen);
}

// Mouse: the engine's cursor position is in client pixels; while the render
// size is managed it must be in backbuffer pixels (where the UI lives).
// The image fills `image` (letterboxed) within the client area.
bool Mapping(void* viewport, RECT& image, LONG& width, LONG& height) {
  width = static_cast<LONG>(g_outWidth.load());
  height = static_cast<LONG>(g_outHeight.load());
  if (width <= 0 || height <= 0) return false;
  RECT client{};
  HWND window = WindowOf(viewport);
  if (!window || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return false;
  image = vrcore::FitRect(width, height, client.right, client.bottom);
  return image.right > image.left && image.bottom > image.top;
}

LONG ToRender(LONG v, LONG imageStart, LONG imageEnd, LONG size) {
  return static_cast<LONG>(static_cast<long long>(v - imageStart) * size / (imageEnd - imageStart));
}

LONG ToClient(LONG v, LONG imageStart, LONG imageEnd, LONG size) {
  return imageStart + static_cast<LONG>(static_cast<long long>(v) * (imageEnd - imageStart) / size);
}

LONG __fastcall HookMouseX(void* viewport, void* /*edx*/) {
  LONG x = g_origMouseX(viewport);
  RECT image;
  LONG w, h;
  if (Mapping(viewport, image, w, h)) x = ToRender(x, image.left, image.right, w);
  return x;
}

LONG __fastcall HookMouseY(void* viewport, void* /*edx*/) {
  LONG y = g_origMouseY(viewport);
  RECT image;
  LONG w, h;
  if (Mapping(viewport, image, w, h)) y = ToRender(y, image.top, image.bottom, h);
  return y;
}

void __fastcall HookMousePos(void* viewport, void* /*edx*/, POINT* out) {
  g_origMousePos(viewport, out);
  RECT image;
  LONG w, h;
  if (out && Mapping(viewport, image, w, h)) {
    out->x = ToRender(out->x, image.left, image.right, w);
    out->y = ToRender(out->y, image.top, image.bottom, h);
  }
}

void __fastcall HookSetMouse(void* viewport, void* /*edx*/, LONG x, LONG y) {
  RECT image;
  LONG w, h;
  if (Mapping(viewport, image, w, h)) {
    x = ToClient(x, image.left, image.right, w);
    y = ToClient(y, image.top, image.bottom, h);
  }
  g_origSetMouse(viewport, x, y);
}

// Makes the game re-run its resize path on its own thread: its window proc
// handles WM_SIZE by resizing the viewport to the configured window size,
// which goes through UpdateViewportRHI (docs/08, "Render size").
void RequestResize(const char* why) {
  HWND window = g_window;
  if (!window) return;
  RECT client{};
  GetClientRect(window, &client);
  if (PostMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom))) {
    g_resizePosted = true;
    VR_LOG_INFO("render size: resizing the game's viewport (%s)", why);
  }
}

// --- Game window (ForceWindowed) ---------------------------------------------

int32_t& SettingsField(void* settings, uint32_t offset) {
  return *reinterpret_cast<int32_t*>(static_cast<char*>(settings) + offset);
}

// A window at the render aspect taking ~3/4 of the primary monitor's work area.
void ComfortableWindow(int32_t& width, int32_t& height) {
  RECT work{0, 0, 1280, 720};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  uint32_t tw = 0, th = 0;
  if (!Target(tw, th)) {
    tw = 16;
    th = 9;
  }
  const RECT r = vrcore::FitRect(static_cast<LONG>(tw), static_cast<LONG>(th), (work.right - work.left) * 3 / 4,
                                 (work.bottom - work.top) * 3 / 4);
  width = std::max<int32_t>(320, (r.right - r.left) & ~1);
  height = std::max<int32_t>(180, r.bottom - r.top);
}

// True if a client area of `width` x `height` (plus a title bar and borders)
// fits the work area.
bool FitsScreen(int32_t width, int32_t height) {
  RECT work{0, 0, 0, 0};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) return true;
  RECT frame{0, 0, width, height};
  AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
  return width > 0 && height > 0 && frame.right - frame.left <= work.right - work.left &&
         frame.bottom - frame.top <= work.bottom - work.top;
}

// After the game loaded ExampleAdriftGfxConfig.ini, before it creates its window.
void __fastcall HookGfxLoad(void* settings, void* /*edx*/) {
  g_origGfxLoad(settings);
  if (!settings || !g_forceWindowed) return;
  int32_t& fullscreen = SettingsField(settings, kGfxSettingsFullscreen);
  int32_t& width = SettingsField(settings, kGfxSettingsResWinWidth);
  int32_t& height = SettingsField(settings, kGfxSettingsResWinHeight);
  const bool wasFullscreen = fullscreen != 0;
  if (!wasFullscreen && FitsScreen(width, height)) {
    VR_LOG_INFO("game window: %dx%d windowed, as configured", width, height);
    return;
  }
  g_patch.origFullscreen = fullscreen;
  g_patch.origWidth = width;
  g_patch.origHeight = height;
  int32_t w = 0, h = 0;
  ComfortableWindow(w, h);
  fullscreen = 0;
  width = w;
  height = h;
  g_patch.fullscreen = 0;
  g_patch.width = w;
  g_patch.height = h;
  g_patch.active = true;
  char was[64] = "fullscreen";
  if (!wasFullscreen) {
    std::snprintf(was, sizeof(was), "%dx%d doesn't fit the screen", g_patch.origWidth, g_patch.origHeight);
  }
  VR_LOG_INFO("game window: %s -> %dx%d windowed (the VR render size needs a window; [VR] ForceWindowed). The "
              "game's config keeps its own values",
              was, w, h);
}

// The game saves its gfx config (at startup when it filled in sizes, or from
// its graphics menu): write the player's values where the mod's are still in place.
void __fastcall HookGfxSave(void* settings, void* /*edx*/) {
  if (!settings || !g_patch.active) {
    g_origGfxSave(settings);
    return;
  }
  int32_t& fullscreen = SettingsField(settings, kGfxSettingsFullscreen);
  int32_t& width = SettingsField(settings, kGfxSettingsResWinWidth);
  int32_t& height = SettingsField(settings, kGfxSettingsResWinHeight);
  const int32_t f = fullscreen, w = width, h = height;
  if (f == g_patch.fullscreen) fullscreen = g_patch.origFullscreen;
  if (w == g_patch.width && h == g_patch.height) {
    width = g_patch.origWidth;
    height = g_patch.origHeight;
  }
  g_origGfxSave(settings);
  fullscreen = f;
  width = w;
  height = h;
}

bool CheckPrologue(uint32_t va, const uint8_t* bytes, size_t size, const char* name) {
  if (vrcore::mem::Matches(reinterpret_cast<uintptr_t>(vrcore::Exe().FromPreferredVA(va)), bytes, size)) return true;
  VR_LOG_ERROR("render size: %s prologue doesn't match this build; window-sized rendering", name);
  return false;
}

// `detour` is the __fastcall stand-in for the __thiscall `original`.
template <typename Detour, typename Fn>
bool Hook(const char* name, uint32_t va, Detour detour, Fn* original) {
  return vrcore::hook::Install(name, vrcore::Exe().FromPreferredVA(va), reinterpret_cast<void*>(detour),
                               reinterpret_cast<void**>(original));
}

}  // namespace

void Configure(const vrcore::Config& config, bool enabled, bool vr) {
  g_enabled = enabled;
  g_vr = vr;
  g_forceWindowed = config.GetBool("VR", "ForceWindowed", true);
  g_disableVSync = config.GetBool("VR", "DisableVSync", true);
  const std::string size = config.GetString("VR", "RenderSize", "auto");
  unsigned w = 0, h = 0;
  if (size == "window") {
    g_mode = Mode::Window;
  } else if (std::sscanf(size.c_str(), "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 16384 && h <= 16384) {
    g_mode = Mode::Fixed;
    g_fixedWidth = w & ~1u;  // two equal halves
    g_fixedHeight = h;
  } else {
    if (size != "auto") VR_LOG_WARN("[VR] RenderSize=%s not understood; using auto", size.c_str());
    g_mode = Mode::Auto;
  }
  g_scale = std::clamp(config.GetFloat("VR", "RenderScale", 1.0f), 0.25f, 2.0f);
}

bool Requested() { return g_enabled && g_mode != Mode::Window; }
bool Managed() { return g_installed && g_outWidth.load() != 0; }

void Install(bool knownBuild) {
  if (!g_enabled || g_mode == Mode::Window || g_installed) {
    if (g_enabled && g_mode == Mode::Window) VR_LOG_INFO("render size: follows the window (RenderSize=window)");
    return;
  }
  if (!knownBuild) {
    VR_LOG_WARN("render size: unknown game build; rendering at the window size");
    return;
  }
  if (!CheckPrologue(kUpdateViewportRhi, kUpdateViewportRhiPrologueA, sizeof(kUpdateViewportRhiPrologueA),
                     "UpdateViewportRHI") ||
      !CheckPrologue(kUpdateViewportRhi + kUpdateViewportRhiPrologueBOffset, kUpdateViewportRhiPrologueB,
                     sizeof(kUpdateViewportRhiPrologueB), "UpdateViewportRHI") ||
      !CheckPrologue(kViewportGetMouseX, kViewportGetMousePrologue, sizeof(kViewportGetMousePrologue), "GetMouseX") ||
      !CheckPrologue(kViewportGetMouseY, kViewportGetMousePrologue, sizeof(kViewportGetMousePrologue), "GetMouseY") ||
      !CheckPrologue(kViewportGetMousePos, kViewportGetMousePrologue, sizeof(kViewportGetMousePrologue),
                     "GetMousePos") ||
      !CheckPrologue(kViewportSetMouse, kViewportSetMousePrologue, sizeof(kViewportSetMousePrologue),
                     "SetMouse")) {
    return;
  }
  if (g_mode == Mode::Auto) {
    uint32_t ew = 0, eh = 0;
    if (vrcore::xr::Context::ProbeRecommendedEyeSize("Remember Me VR", ew, eh)) {
      g_eyeWidth = ew;
      g_eyeHeight = eh;
    } else {
      VR_LOG_WARN("render size: no headset found at startup; window size until the VR session starts");
    }
  }
  // Mouse first: once the viewport is resized, positions must be mapped.
  g_installed = Hook("GetMouseX", kViewportGetMouseX, &HookMouseX, &g_origMouseX) &&
                Hook("GetMouseY", kViewportGetMouseY, &HookMouseY, &g_origMouseY) &&
                Hook("GetMousePos", kViewportGetMousePos, &HookMousePos, &g_origMousePos) &&
                Hook("SetMouse", kViewportSetMouse, &HookSetMouse, &g_origSetMouse) &&
                Hook("UpdateViewportRHI", kUpdateViewportRhi, &HookUpdateViewportRhi, &g_origUpdate);
  // The game window: windowed, at a size that fits (before UGameEngine::Init loads the settings).
  if (g_installed && g_forceWindowed) {
    const bool ok =
        CheckPrologue(kGfxSettingsLoad, kGfxSettingsLoadPrologueA, sizeof(kGfxSettingsLoadPrologueA), "gfx load") &&
        CheckPrologue(kGfxSettingsLoad + kGfxSettingsLoadPrologueBOffset, kGfxSettingsLoadPrologueB,
                      sizeof(kGfxSettingsLoadPrologueB), "gfx load") &&
        CheckPrologue(kGfxSettingsSave, kGfxSettingsSavePrologueA, sizeof(kGfxSettingsSavePrologueA), "gfx save") &&
        CheckPrologue(kGfxSettingsSave + kGfxSettingsSavePrologueBOffset, kGfxSettingsSavePrologueB,
                      sizeof(kGfxSettingsSavePrologueB), "gfx save") &&
        Hook("gfx settings save", kGfxSettingsSave, &HookGfxSave, &g_origGfxSave) &&
        Hook("gfx settings load", kGfxSettingsLoad, &HookGfxLoad, &g_origGfxLoad);
    if (!ok) VR_LOG_WARN("game window: hooks failed; the game's fullscreen/window settings apply");
  }
  uint32_t w = 0, h = 0;
  if (!Target(w, h)) {
    VR_LOG_INFO("render size hooks %s; size not known yet", g_installed ? "installed" : "FAILED");
  } else if (g_mode == Mode::Auto) {
    VR_LOG_INFO("render size hooks %s; %ux%u (headset eyes %ux%u x RenderScale %.2f, side by side)",
                g_installed ? "installed" : "FAILED", w, h, g_eyeWidth.load(), g_eyeHeight.load(), g_scale);
  } else {
    VR_LOG_INFO("render size hooks %s; %ux%u (RenderSize)", g_installed ? "installed" : "FAILED", w, h);
  }
}

bool ForceWindowed() { return g_forceWindowed; }
void SetForceWindowed(bool on) { g_forceWindowed = on; }
bool DisableVSync() { return g_disableVSync; }
void SetDisableVSync(bool on) { g_disableVSync = on; }

void Save(const vrcore::Config& config) {
  config.SetBool("VR", "ForceWindowed", g_forceWindowed);
  config.SetBool("VR", "DisableVSync", g_disableVSync);
}

void AdjustPresentParameters(D3DPRESENT_PARAMETERS& params) {
  // UE3's windowed D3D9 backbuffer is the larger of the window and the
  // viewport, and never shrinks; it renders the viewport into the top-left and
  // presents that rect. A render size smaller than the window (RenderScale < 1)
  // would leave the backbuffer wider than both eyes, while the compositor,
  // mirror and UI redirect split it at half its width. Make it exactly the
  // viewport.
  const uint32_t vw = g_outWidth, vh = g_outHeight;
  if (params.Windowed && vw && vh && (params.BackBufferWidth != vw || params.BackBufferHeight != vh)) {
    VR_LOG_INFO("render size: backbuffer %ux%u -> %ux%u (the engine viewport)", params.BackBufferWidth,
                params.BackBufferHeight, vw, vh);
    params.BackBufferWidth = vw;
    params.BackBufferHeight = vh;
  }
  if (!g_vr || !g_disableVSync || params.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE) return;
  static bool logged = false;
  if (!logged) {
    logged = true;
    VR_LOG_INFO("vsync off while VR runs (game asked for present interval 0x%lx; [VR] DisableVSync)",
                static_cast<unsigned long>(params.PresentationInterval));
  }
  params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

void OnEyeSize(uint32_t eyeWidth, uint32_t eyeHeight) {
  if (!g_installed || g_mode != Mode::Auto || !eyeWidth || !eyeHeight) return;
  if (eyeWidth == g_eyeWidth && eyeHeight == g_eyeHeight) return;
  g_eyeWidth = eyeWidth;
  g_eyeHeight = eyeHeight;
  RequestResize("headset eye size now known");
}

void OnDevice(HWND window, uint32_t backbufferWidth, uint32_t backbufferHeight) {
  if (!g_installed) return;
  if (window && !g_window.load()) g_window = window;
  uint32_t w = 0, h = 0;
  // The hook missed the viewport's creation (Direct3DCreate9 came late): once.
  if (Target(w, h) && g_outWidth == 0 && !g_resizePosted) {
    VR_LOG_INFO("render size: backbuffer %ux%u was sized before the hook", backbufferWidth, backbufferHeight);
    RequestResize("hooked after the viewport was created");
  }
}

}  // namespace rm::render_size
