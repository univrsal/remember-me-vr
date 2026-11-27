#include "menu.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "free_cam.h"
#include "hide_actors.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "input_capture.h"
#include "mirror.h"
#include "mouse_unlock.h"
#include "render_size.h"
#include "sensen_stereo.h"
#include "vr_gamepad.h"
#include "vrcore/log.h"
#include "vrcore/process.h"
#include "vrcore/scene_tap.h"

namespace rm::menu {
namespace {

constexpr UINT kWidth = 1280;  // menu texture (VR panel aspect follows it)
constexpr UINT kHeight = 960;

Targets g_targets;
float g_cursorSpeed = 20.0f;  // [Menu] CursorSpeed: menu pixels per mouse count
bool g_imguiReady = false;
bool g_backendReady = false;
IDirect3DDevice9* g_device = nullptr;  // identity only
IDirect3DTexture9* g_texture = nullptr;  // D3DPOOL_DEFAULT render target
IDirect3DSurface9* g_surface = nullptr;
bool g_renderedThisFrame = false;
bool g_wasOpen = false;
ImVec2 g_cursor{kWidth * 0.5f, kHeight * 0.5f};
LARGE_INTEGER g_lastTick{};
int g_laserHand = 1;            // controller whose laser drives the cursor (last to click)
bool g_laserOnPanel = false;
bool g_keysDown[8] = {};
bool g_padButtonsDown[16] = {};
char g_status[128] = "";
bool g_dumpPending = false;
int g_selectTab = -1;  // [Debug] DumpMenuTab: tab selected on the first frame  // [Debug] DumpMenu: write the first rendered menu to a BMP
uint64_t g_statusUntilMs = 0;

// --- Setup -----------------------------------------------------------------
void InitImGui() {
  if (g_imguiReady) return;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // no imgui.ini next to the game
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.MouseDrawCursor = true;  // software cursor: the OS one is hidden in game and absent in VR
  io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));

  // Big enough to read on the VR panel. A Windows TTF if there is one (Wine
  // prefixes usually have some), else ImGui's bitmap font scaled up.
  constexpr float kFontSize = 28.0f;
  wchar_t windows[MAX_PATH] = L"";
  GetWindowsDirectoryW(windows, MAX_PATH);
  bool font = false;
  for (const wchar_t* name : {L"\\Fonts\\segoeui.ttf", L"\\Fonts\\arial.ttf", L"\\Fonts\\tahoma.ttf",
                              L"\\Fonts\\verdana.ttf"}) {
    char path[MAX_PATH * 2];
    std::snprintf(path, sizeof(path), "%ls%ls", windows, name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
    if (io.Fonts->AddFontFromFileTTF(path, kFontSize)) {
      VR_LOG_INFO("menu: font %s", path);
      font = true;
      break;
    }
  }
  if (!font) {
    ImFontConfig config;
    config.SizePixels = kFontSize;
    io.Fonts->AddFontDefault(&config);
    VR_LOG_INFO("menu: no TTF font found; ImGui's default font scaled up");
  }
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.ScaleAllSizes(2.0f);
  style.WindowRounding = 8.0f;
  style.FrameRounding = 6.0f;
  style.GrabMinSize = 24.0f;
  style.Colors[ImGuiCol_WindowBg].w = 0.94f;
  g_imguiReady = true;
}

bool EnsureTarget(IDirect3DDevice9* device) {
  if (device != g_device) {
    // A new device: the old one's objects are gone with it.
    if (g_backendReady) ImGui_ImplDX9_Shutdown();
    g_backendReady = false;
    g_texture = nullptr;
    g_surface = nullptr;
    g_device = device;
  }
  if (!g_backendReady) {
    if (!ImGui_ImplDX9_Init(device)) return false;
    g_backendReady = true;
  }
  if (g_texture) return true;
  if (FAILED(device->CreateTexture(kWidth, kHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                   &g_texture, nullptr)) ||
      !g_texture || FAILED(g_texture->GetSurfaceLevel(0, &g_surface))) {
    VR_LOG_ERROR("menu: creating the %ux%u render target failed", kWidth, kHeight);
    ReleaseDeviceResources();
    return false;
  }
  return true;
}

// Debug: the menu texture as RememberMeVR_menu.bmp next to the DLL.
void DumpTexture(IDirect3DDevice9* device) {
  IDirect3DSurface9* copy = nullptr;
  if (FAILED(device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr)) ||
      !copy) {
    return;
  }
  D3DLOCKED_RECT lock{};
  if (SUCCEEDED(device->GetRenderTargetData(g_surface, copy)) && SUCCEEDED(copy->LockRect(&lock, nullptr, D3DLOCK_READONLY))) {
    const std::wstring path = vrcore::ModDirectory() + L"RememberMeVR_menu.bmp";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
      BITMAPFILEHEADER fh{};
      BITMAPINFOHEADER ih{};
      ih.biSize = sizeof(ih);
      ih.biWidth = kWidth;
      ih.biHeight = -static_cast<LONG>(kHeight);  // top-down
      ih.biPlanes = 1;
      ih.biBitCount = 32;
      fh.bfType = 0x4D42;
      fh.bfOffBits = sizeof(fh) + sizeof(ih);
      fh.bfSize = fh.bfOffBits + kWidth * kHeight * 4;
      fwrite(&fh, sizeof(fh), 1, f);
      fwrite(&ih, sizeof(ih), 1, f);
      for (UINT y = 0; y < kHeight; ++y) fwrite(static_cast<const char*>(lock.pBits) + y * lock.Pitch, 4, kWidth, f);
      fclose(f);
      VR_LOG_INFO("menu: dumped to %s", vrcore::Narrow(path).c_str());
    }
    copy->UnlockRect();
  }
  copy->Release();
}

// --- Input -----------------------------------------------------------------
XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
  const XrVector3f u{q.x, q.y, q.z};
  const XrVector3f t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
  return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
          v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

// Where the controller's aim ray hits the panel, in menu pixels.
bool LaserHit(const XrPosef& aim, const XrPosef& panel, const XrExtent2Df& size, ImVec2& out) {
  const XrQuaternionf inv{-panel.orientation.x, -panel.orientation.y, -panel.orientation.z, panel.orientation.w};
  const XrVector3f rel{aim.position.x - panel.position.x, aim.position.y - panel.position.y,
                       aim.position.z - panel.position.z};
  const XrVector3f o = Rotate(inv, rel);
  const XrVector3f d = Rotate(inv, Rotate(aim.orientation, {0.0f, 0.0f, -1.0f}));
  if (d.z >= -1e-4f || o.z <= 0.0f) return false;  // pointing away, or behind the panel
  const float t = -o.z / d.z;
  const float u = (o.x + t * d.x) / size.width + 0.5f;
  const float v = 0.5f - (o.y + t * d.y) / size.height;
  if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
  out = ImVec2(u * kWidth, v * kHeight);
  return true;
}

void KeyEdge(ImGuiIO& io, int slot, bool down, ImGuiKey key) {
  if (down != g_keysDown[slot]) io.AddKeyEvent(key, down);
  g_keysDown[slot] = down;
}

void FeedInput(ImGuiIO& io) {
  const float dx = input_capture::TakeMouseX(), dy = input_capture::TakeMouseY();
  g_cursor.x = std::clamp(g_cursor.x + dx * g_cursorSpeed, 0.0f, static_cast<float>(kWidth - 1));
  g_cursor.y = std::clamp(g_cursor.y - dy * g_cursorSpeed, 0.0f, static_cast<float>(kHeight - 1));  // MouseY: up = +
  bool click = input_capture::MouseButton(0);
  float wheel = input_capture::TakeWheel();

  // VR laser.
  g_laserOnPanel = false;
  XrPosef panel{};
  XrExtent2Df size{};
  if (g_targets.compositor && g_targets.compositor->MenuPanel(panel, size)) {
    const vrcore::xr::ControllerState c = g_targets.compositor->Controllers();
    if (c.active) {
      for (int h = 0; h < 2; ++h) {
        if (c.trigger[h] > 0.6f) g_laserHand = h;
      }
      ImVec2 hit;
      for (int h : {g_laserHand, 1 - g_laserHand}) {
        if (!c.aimValid[h] || !LaserHit(c.aim[h], panel, size, hit)) continue;
        g_laserHand = h;
        g_cursor = hit;
        g_laserOnPanel = true;
        click = click || c.trigger[h] > 0.6f;
        wheel += c.stick[h][1] * 0.25f;  // thumbstick scrolls
        break;
      }
    }
  }
  io.AddMousePosEvent(g_cursor.x, g_cursor.y);
  io.AddMouseButtonEvent(0, click);
  io.AddMouseButtonEvent(1, input_capture::MouseButton(1));
  if (wheel != 0.0f) io.AddMouseWheelEvent(0.0f, wheel);

  // Keyboard navigation.
  const bool focused = input_capture::GameWindowFocused();
  const auto key = [&](int vk) { return focused && (GetAsyncKeyState(vk) & 0x8000) != 0; };
  KeyEdge(io, 0, key(VK_UP), ImGuiKey_UpArrow);
  KeyEdge(io, 1, key(VK_DOWN), ImGuiKey_DownArrow);
  KeyEdge(io, 2, key(VK_LEFT), ImGuiKey_LeftArrow);
  KeyEdge(io, 3, key(VK_RIGHT), ImGuiKey_RightArrow);
  KeyEdge(io, 4, key(VK_RETURN), ImGuiKey_Enter);
  KeyEdge(io, 5, key(VK_SPACE), ImGuiKey_Space);
  KeyEdge(io, 6, key(VK_TAB), ImGuiKey_Tab);
  if (key(VK_ESCAPE) && !g_keysDown[7]) input_capture::SetMenuOpen(false);
  g_keysDown[7] = key(VK_ESCAPE);

  // Gamepad navigation (VR controllers included; the laser trigger is RT, unused here).
  XINPUT_GAMEPAD pad{};
  const bool havePad = vr_gamepad::ReadPad(pad);
  const struct {
    WORD button;
    ImGuiKey key;
  } buttons[] = {{XINPUT_GAMEPAD_DPAD_UP, ImGuiKey_GamepadDpadUp},
                 {XINPUT_GAMEPAD_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
                 {XINPUT_GAMEPAD_DPAD_LEFT, ImGuiKey_GamepadDpadLeft},
                 {XINPUT_GAMEPAD_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
                 {XINPUT_GAMEPAD_A, ImGuiKey_GamepadFaceDown},
                 {XINPUT_GAMEPAD_B, ImGuiKey_GamepadFaceRight},
                 {XINPUT_GAMEPAD_X, ImGuiKey_GamepadFaceLeft},
                 {XINPUT_GAMEPAD_Y, ImGuiKey_GamepadFaceUp},
                 {XINPUT_GAMEPAD_LEFT_SHOULDER, ImGuiKey_GamepadL1},
                 {XINPUT_GAMEPAD_RIGHT_SHOULDER, ImGuiKey_GamepadR1}};
  for (size_t i = 0; i < std::size(buttons); ++i) {
    const bool down = havePad && (pad.wButtons & buttons[i].button) != 0;
    if (down != g_padButtonsDown[i]) io.AddKeyEvent(buttons[i].key, down);
    g_padButtonsDown[i] = down;
  }
  // Start closes the menu (Index: right trackpad press).
  const bool start = havePad && (pad.wButtons & XINPUT_GAMEPAD_START) != 0;
  if (start && !g_padButtonsDown[15]) input_capture::SetMenuOpen(false);
  g_padButtonsDown[15] = start;
  if (!g_laserOnPanel) {
    // Left stick navigates (not while the laser points at the panel: Index sticks double as scroll).
    const auto axis = [](SHORT v) { return std::clamp(v / 32767.0f, -1.0f, 1.0f); };
    const float lx = havePad ? axis(pad.sThumbLX) : 0.0f, ly = havePad ? axis(pad.sThumbLY) : 0.0f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, lx < -0.3f, std::max(0.0f, -lx));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx > 0.3f, std::max(0.0f, lx));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ly > 0.3f, std::max(0.0f, ly));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ly < -0.3f, std::max(0.0f, -ly));
  }
}

// --- Content ---------------------------------------------------------------
void Status(const char* text) {
  std::snprintf(g_status, sizeof(g_status), "%s", text);
  g_statusUntilMs = GetTickCount64() + 4000;
}

void Save() {
  const vrcore::Config& config = g_targets.config;
  bool ok = true;
  free_cam::Save(config);
  hide_actors::Save(config);
  mouse_unlock::Save(config);
  config.SetFloat("Menu", "CursorSpeed", g_cursorSpeed);
  config.SetBool("Visibility", "HideSensen", sensen_stereo::Hidden());
  if (g_targets.sceneViews) g_targets.sceneViews->Save(config);
  if (g_targets.disableMotionBlur) config.SetBool("VR", "DisableMotionBlur", g_targets.disableMotionBlur->load());
  config.Set("VR", "MirrorView", mirror::GetView() == mirror::View::Stereo ? "stereo" : "left");
  config.SetBool("VR", "MirrorUi", vrcore::scene_tap::UiMirrorEnabled());
  render_size::Save(config);
  if (g_targets.compositor) {
    const auto& s = g_targets.compositor->MutableSettings();
    config.SetFloat("VR", "UiDistance", s.uiDistance);
    config.SetFloat("VR", "UiWidth", s.uiWidth);
    config.SetFloat("VR", "UiHeight", s.uiHeight);
    config.SetFloat("VR", "ScreenDistance", s.screenDistance);
    config.SetFloat("VR", "ScreenWidth", s.screenWidth);
    config.SetFloat("VR", "MenuDistance", s.menuDistance);
    config.SetFloat("VR", "MenuWidth", s.menuWidth);
  }
  ok = config.Exists();
  Status(ok ? "Saved to RememberMeVR.ini" : "Saving failed (INI not found)");
  VR_LOG_INFO("menu: settings %s", ok ? "saved" : "NOT saved (INI missing)");
}

void Help(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

void CameraTab() {
  bool freeCam = free_cam::Active();
  if (ImGui::Checkbox("Free camera", &freeCam)) {
    free_cam::SetActive(freeCam);
    if (freeCam) input_capture::SetMenuOpen(false);  // straight into flying
  }
  Help("Fly the view while the game keeps running (the player gets no input).\n"
       "WASD/E/Q + mouse, or sticks + triggers; RB/Shift fast, LB/Ctrl slow.");
  auto& f = free_cam::Tunables();
  ImGui::SliderFloat("Speed (uu/s)", &f.speed, 50.0f, 3000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("Fast multiplier", &f.fastMultiplier, 1.0f, 10.0f, "%.1f");
  ImGui::SliderFloat("Slow multiplier", &f.slowMultiplier, 0.05f, 1.0f, "%.2f");
  ImGui::SliderFloat("Stick look speed (deg/s)", &f.lookSpeed, 30.0f, 360.0f, "%.0f");
  ImGui::SliderFloat("Mouse sensitivity", &f.mouseSensitivity, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
  ImGui::Checkbox("Invert mouse Y", &f.invertMouseY);
  ImGui::SliderFloat("Stick deadzone", &f.deadzone, 0.0f, 0.9f, "%.2f");
}

void VisibilityTab() {
  ImGui::BeginDisabled(!hide_actors::Available());
  bool player = hide_actors::HidePlayer(), npcs = hide_actors::HideNpcs();
  if (ImGui::Checkbox("Hide player", &player)) hide_actors::SetHidePlayer(player);
  if (ImGui::Checkbox("Hide NPCs and enemies", &npcs)) hide_actors::SetHideNpcs(npcs);
  Help("Pawns and crowd agents, and what's attached to them.");
  bool skeletal = hide_actors::HideSkeletalMeshes();
  if (ImGui::Checkbox("Hide placed skeletal meshes", &skeletal)) hide_actors::SetHideSkeletalMeshes(skeletal);
  Help("Characters placed in the level that aren't NPC pawns (e.g. standing bystanders),\n"
       "but also animated props such as doors or machinery.");
  ImGui::EndDisabled();
  if (!hide_actors::Available()) ImGui::TextDisabled("(needs the engine hooks and reflection)");
  ImGui::BeginDisabled(!sensen_stereo::Available());
  bool sensen = sensen_stereo::Hidden();
  if (ImGui::Checkbox("Hide Sensen hints (in-world prompts)", &sensen)) sensen_stereo::SetHidden(sensen);
  ImGui::EndDisabled();
  if (!sensen_stereo::Available()) ImGui::TextDisabled("(needs VR with StereoSource=scene)");
}

void VrTab() {
  vrcore::Compositor* c = g_targets.compositor;
  SceneViewHooks* sv = g_targets.sceneViews;
  if (!c) {
    ImGui::TextDisabled("VR is off ([VR] Enabled=0).");
    return;
  }
  if (ImGui::Button("Recenter view")) {
    c->RequestRecenter();
    Status("Recentred");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(F8)");
  if (sv) {
    float scale = sv->WorldScale();
    if (ImGui::SliderFloat("World scale (uu per metre)", &scale, 10.0f, 200.0f, "%.0f")) sv->SetWorldScale(scale);
    Help("Larger = you feel smaller.");
    int orientation = sv->FullOrientation() ? 1 : 0;
    if (ImGui::Combo("Camera orientation", &orientation, "Yaw only (level horizon)\0Full (follow game pitch/roll)\0")) {
      sv->SetFullOrientation(orientation == 1);
    }
    bool symmetric = sv->SymmetricProjection();
    if (ImGui::Checkbox("Symmetric projection", &symmetric)) sv->SetSymmetricProjection(symmetric);
    Help("Workaround for effects that assume a centred projection (about 10% more pixels).");
    bool flatLoading = sv->FlatLoadingScreens(), flatMenus = sv->FlatMenus();
    if (ImGui::Checkbox("Loading screens on the flat panel", &flatLoading)) sv->SetFlatLoadingScreens(flatLoading);
    if (ImGui::Checkbox("Game menus on the flat panel", &flatMenus)) sv->SetFlatMenus(flatMenus);
  }
  auto& s = c->MutableSettings();
  ImGui::SeparatorText("Panels (metres)");
  ImGui::SliderFloat("HUD distance", &s.uiDistance, 0.5f, 5.0f, "%.2f");
  ImGui::SliderFloat("HUD width", &s.uiWidth, 0.3f, 4.0f, "%.2f");
  ImGui::SliderFloat("HUD height offset", &s.uiHeight, -1.5f, 1.5f, "%.2f");
  ImGui::SliderFloat("Flat panel distance", &s.screenDistance, 0.5f, 10.0f, "%.2f");
  ImGui::SliderFloat("Flat panel width", &s.screenWidth, 0.5f, 10.0f, "%.2f");
  ImGui::SliderFloat("Menu distance", &s.menuDistance, 0.4f, 3.0f, "%.2f");
  ImGui::SliderFloat("Menu width", &s.menuWidth, 0.3f, 3.0f, "%.2f");
  ImGui::TextDisabled("Headset FOV comes from the headset; it can't be changed here.");
}

void DisplayTab() {
  ImGui::BeginDisabled(!mouse_unlock::Available());
  bool freeMouse = mouse_unlock::Enabled();
  if (ImGui::Checkbox("Free the mouse cursor in game menus", &freeMouse)) mouse_unlock::SetEnabled(freeMouse);
  Help("Pause, Sensen and main menu: show the cursor and let it leave the window.");
  ImGui::EndDisabled();
  ImGui::SliderFloat("Menu cursor speed", &g_cursorSpeed, 1.0f, 60.0f, "%.1f");
  if (g_targets.disableMotionBlur) {
    bool off = g_targets.disableMotionBlur->load();
    if (ImGui::Checkbox("Disable motion blur", &off)) *g_targets.disableMotionBlur = off;
  }
  if (g_targets.compositor) {
    int view = mirror::GetView() == mirror::View::Stereo ? 1 : 0;
    if (ImGui::Combo("Window shows", &view, "Left eye\0Both eyes\0")) {
      mirror::SetView(view == 1 ? mirror::View::Stereo : mirror::View::Left);
    }
    bool mirrorUi = vrcore::scene_tap::UiMirrorEnabled();
    if (ImGui::Checkbox("Draw the HUD into the window", &mirrorUi)) vrcore::scene_tap::EnableUiMirror(mirrorUi);
  }
  ImGui::SeparatorText("Game window (takes effect at the next start; Save first)");
  bool windowed = render_size::ForceWindowed();
  if (ImGui::Checkbox("Start windowed in VR", &windowed)) render_size::SetForceWindowed(windowed);
  Help("Overrides the game's fullscreen setting while VR runs, with a window that fits the screen:\n"
       "the VR resolution only applies to a window. The game's own config is left as it is.");
  bool noVSync = render_size::DisableVSync();
  if (ImGui::Checkbox("Disable vsync in VR", &noVSync)) render_size::SetDisableVSync(noVSync);
  Help("The headset sets the pace; with vsync the monitor's refresh rate (e.g. 60 Hz) would cap it.");
}

void Build() {
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight)));
  ImGui::Begin("Remember Me VR", nullptr,
               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoSavedSettings);
  // Tab contents scroll above the footer (laser: thumbstick, mouse wheel, D-pad).
  const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
  if (ImGui::BeginTabBar("tabs")) {
    const struct {
      const char* name;
      void (*draw)();
    } tabs[] = {{"Camera", CameraTab}, {"Visibility", VisibilityTab}, {"VR", VrTab}, {"Display", DisplayTab}};
    for (int i = 0; i < static_cast<int>(std::size(tabs)); ++i) {
      const auto& tab = tabs[i];
      const ImGuiTabItemFlags flags = i == g_selectTab ? ImGuiTabItemFlags_SetSelected : 0;
      if (!ImGui::BeginTabItem(tab.name, nullptr, flags)) continue;
      ImGui::BeginChild("content", ImVec2(0.0f, -footer), ImGuiChildFlags_NavFlattened);
      tab.draw();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    g_selectTab = -1;
  }
  ImGui::Separator();
  if (ImGui::Button("Save")) Save();
  ImGui::SameLine();
  if (ImGui::Button("Close")) input_capture::SetMenuOpen(false);
  ImGui::SameLine();
  if (GetTickCount64() < g_statusUntilMs) {
    ImGui::TextUnformatted(g_status);
  } else {
    ImGui::TextDisabled("F6 / both stick clicks: close   Laser + trigger, mouse, D-pad + A");
  }
  ImGui::End();
}

// Renders the current draw data into the bound target at (x, y).
void RenderDrawData(float x, float y, float width, float height) {
  ImDrawData* data = ImGui::GetDrawData();
  if (!data) return;
  const ImVec2 pos = data->DisplayPos, size = data->DisplaySize;
  // The backend maps DisplayPos..DisplayPos+DisplaySize onto a viewport at 0,0:
  // shift the origin to draw at (x, y) of a larger target.
  data->DisplayPos = ImVec2(pos.x - x, pos.y - y);
  data->DisplaySize = ImVec2(width, height);
  // Plain (non-sRGB) writes and reads whatever the game left set; the backend
  // doesn't touch these two.
  IDirect3DDevice9* device = g_device;
  DWORD srgbWrite = FALSE, srgbTexture = FALSE;
  device->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbWrite);
  device->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgbTexture);
  device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  ImGui_ImplDX9_RenderDrawData(data);
  device->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite);
  device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, srgbTexture);
  data->DisplayPos = pos;
  data->DisplaySize = size;
}

}  // namespace

void Configure(const vrcore::Config& config, const Targets& targets) {
  g_targets = targets;
  g_targets.config = config;
  g_dumpPending = config.GetBool("Debug", "DumpMenu", false);
  g_cursorSpeed = config.GetFloat("Menu", "CursorSpeed", g_cursorSpeed);
  g_selectTab = config.GetInt("Debug", "DumpMenuTab", -1);
}

IDirect3DTexture9* Render(IDirect3DDevice9* device) {
  g_renderedThisFrame = false;
  const bool open = input_capture::MenuOpen();
  if (open && !g_wasOpen) g_cursor = ImVec2(kWidth * 0.5f, kHeight * 0.5f);
  g_wasOpen = open;
  if (!open) return nullptr;
  InitImGui();
  if (!EnsureTarget(device)) return nullptr;

  ImGuiIO& io = ImGui::GetIO();
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  io.DeltaTime = g_lastTick.QuadPart
                     ? std::clamp(static_cast<float>(now.QuadPart - g_lastTick.QuadPart) / freq.QuadPart, 1e-4f, 0.25f)
                     : 1.0f / 60.0f;
  g_lastTick = now;
  FeedInput(io);
  ImGui_ImplDX9_NewFrame();
  ImGui::NewFrame();
  Build();
  ImGui::Render();

  // Into the menu texture, untouched by the scene/UI hooks; the game's render
  // target, depth buffer and viewport are restored afterwards (the backend
  // restores the rest with a state block).
  vrcore::scene_tap::Bypass bypass;
  IDirect3DSurface9* oldTarget = nullptr;
  IDirect3DSurface9* oldDepth = nullptr;
  D3DVIEWPORT9 oldViewport{};
  device->GetRenderTarget(0, &oldTarget);
  device->GetDepthStencilSurface(&oldDepth);
  device->GetViewport(&oldViewport);
  device->SetRenderTarget(0, g_surface);
  device->SetDepthStencilSurface(nullptr);
  device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
  RenderDrawData(0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight));
  device->SetRenderTarget(0, oldTarget);
  device->SetDepthStencilSurface(oldDepth);
  device->SetViewport(&oldViewport);
  if (oldTarget) oldTarget->Release();
  if (oldDepth) oldDepth->Release();
  g_renderedThisFrame = true;
  // A few frames in, so the layout has settled.
  static int frames = 0;
  if (g_dumpPending && ++frames == 10) {
    g_dumpPending = false;
    DumpTexture(device);
  }
  return g_texture;
}

void DrawToWindow(IDirect3DDevice9* device) {
  if (!g_renderedThisFrame) return;
  vrcore::scene_tap::Bypass bypass;
  IDirect3DSurface9* oldTarget = nullptr;
  IDirect3DSurface9* backbuffer = nullptr;
  IDirect3DSurface9* oldDepth = nullptr;
  D3DVIEWPORT9 oldViewport{};
  D3DSURFACE_DESC desc{};
  if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) || !backbuffer ||
      FAILED(backbuffer->GetDesc(&desc))) {
    if (backbuffer) backbuffer->Release();
    return;
  }
  device->GetRenderTarget(0, &oldTarget);
  device->GetDepthStencilSurface(&oldDepth);
  device->GetViewport(&oldViewport);
  device->SetRenderTarget(0, backbuffer);
  device->SetDepthStencilSurface(nullptr);
  const float x = std::max(0.0f, (static_cast<float>(desc.Width) - kWidth) * 0.5f);
  const float y = std::max(0.0f, (static_cast<float>(desc.Height) - kHeight) * 0.5f);
  RenderDrawData(x, y, static_cast<float>(desc.Width), static_cast<float>(desc.Height));
  device->SetRenderTarget(0, oldTarget);
  device->SetDepthStencilSurface(oldDepth);
  device->SetViewport(&oldViewport);
  if (oldTarget) oldTarget->Release();
  if (oldDepth) oldDepth->Release();
  backbuffer->Release();
}

void ReleaseDeviceResources() {
  if (g_backendReady) ImGui_ImplDX9_InvalidateDeviceObjects();  // recreated by the next NewFrame
  if (g_surface) g_surface->Release();
  if (g_texture) g_texture->Release();
  g_surface = nullptr;
  g_texture = nullptr;
}

}  // namespace rm::menu
