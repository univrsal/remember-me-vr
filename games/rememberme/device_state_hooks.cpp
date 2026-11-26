#include "device_state_hooks.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vrcore/hook.h"
#include "vrcore/log.h"

namespace rm::device_state {
namespace {

// IDirect3DDevice9 vtable slots (d3d9.h declaration order).
constexpr size_t kSetViewport = 47;
constexpr size_t kSetTexture = 65;
constexpr size_t kSetSamplerState = 69;
constexpr size_t kCreateVertexShader = 91;
constexpr size_t kSetVertexShader = 92;
constexpr size_t kSetVertexShaderConstantF = 94;
constexpr size_t kCreatePixelShader = 106;
constexpr size_t kSetPixelShader = 107;
constexpr size_t kSetPixelShaderConstantF = 109;

using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetSamplerState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using SetPixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetConstantF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using CreateVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using CreatePixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);

SetViewport_t g_origSetViewport = nullptr;
SetTexture_t g_origSetTexture = nullptr;
SetSamplerState_t g_origSetSamplerState = nullptr;
SetPixelShader_t g_origSetPixelShader = nullptr;
SetConstantF_t g_origSetVertexShaderConstantF = nullptr;
SetConstantF_t g_origSetPixelShaderConstantF = nullptr;
CreateVertexShader_t g_origCreateVertexShader = nullptr;
SetVertexShader_t g_origSetVertexShader = nullptr;
CreatePixelShader_t g_origCreatePixelShader = nullptr;

// Float constants of a shader, from its CTAB comment block: name -> first register.
using ShaderConstants = std::vector<std::pair<std::string, int>>;
std::mutex g_shadersMutex;
std::unordered_map<const void*, std::shared_ptr<const ShaderConstants>> g_shaders;

Options g_options;
std::atomic<bool> g_installed{false};
std::atomic<uint64_t> g_fixedCount{0};

// Render-thread state.
thread_local D3DVIEWPORT9 t_viewport{0, 0, 0, 0, 0.0f, 1.0f};
thread_local bool t_trace = false;
thread_local std::vector<float> t_scratch;
thread_local std::shared_ptr<const ShaderConstants> t_vs, t_ps;
thread_local IDirect3DBaseTexture9* t_stage0 = nullptr;  // identity only (bound, so alive)

// Parses the CTAB comment of SM2/3 bytecode into float-register constants.
ShaderConstants ParseConstantTable(const DWORD* code) {
  ShaderConstants out;
  if (!code) return out;
  for (size_t i = 1; i < 1 << 16; ) {  // token 0 is the version
    const DWORD token = code[i];
    if (token == 0x0000FFFF) break;    // end
    if ((token & 0xFFFF) != 0xFFFE) break;  // the CTAB comment comes first, before any instruction
    const DWORD length = (token >> 16) & 0x7FFF;
    const auto* body = reinterpret_cast<const uint8_t*>(code + i + 1);
    const size_t bytes = length * 4;
    if (bytes >= 32 && std::memcmp(body, "CTAB", 4) == 0) {
      const uint8_t* ctab = body + 4;
      const size_t size = bytes - 4;
      uint32_t count = 0, info = 0;
      std::memcpy(&count, ctab + 12, 4);
      std::memcpy(&info, ctab + 16, 4);
      for (uint32_t k = 0; k < count && info + (k + 1) * 20 <= size; ++k) {
        uint32_t nameOffset = 0;
        uint16_t regSet = 0, regIndex = 0;
        std::memcpy(&nameOffset, ctab + info + k * 20, 4);
        std::memcpy(&regSet, ctab + info + k * 20 + 4, 2);
        std::memcpy(&regIndex, ctab + info + k * 20 + 6, 2);
        if (regSet != 2 || nameOffset >= size) continue;  // 2 = D3DXRS_FLOAT4
        const char* name = reinterpret_cast<const char*>(ctab + nameOffset);
        out.emplace_back(std::string(name, strnlen(name, size - nameOffset)), regIndex);
      }
      break;
    }
    i += length + 1;
  }
  return out;
}

void Remember(const void* shader, const DWORD* code) {
  if (!shader) return;
  auto constants = std::make_shared<const ShaderConstants>(ParseConstantTable(code));
  std::lock_guard<std::mutex> lock(g_shadersMutex);
  g_shaders[shader] = std::move(constants);
}

std::shared_ptr<const ShaderConstants> Lookup(const void* shader) {
  if (!shader) return nullptr;
  std::lock_guard<std::mutex> lock(g_shadersMutex);
  auto it = g_shaders.find(shader);
  return it == g_shaders.end() ? nullptr : it->second;
}

int FindConstant(const std::shared_ptr<const ShaderConstants>& shader, const char* name) {
  if (!shader) return -1;
  for (const auto& [n, reg] : *shader) {
    if (n == name) return reg;
  }
  return -1;
}

const char* SamplerStateName(D3DSAMPLERSTATETYPE t) {
  switch (t) {
    case D3DSAMP_ADDRESSU: return "ADDRESSU";
    case D3DSAMP_ADDRESSV: return "ADDRESSV";
    case D3DSAMP_ADDRESSW: return "ADDRESSW";
    case D3DSAMP_BORDERCOLOR: return "BORDERCOLOR";
    case D3DSAMP_MAGFILTER: return "MAGFILTER";
    case D3DSAMP_MINFILTER: return "MINFILTER";
    case D3DSAMP_MIPFILTER: return "MIPFILTER";
    case D3DSAMP_MIPMAPLODBIAS: return "MIPMAPLODBIAS";
    case D3DSAMP_MAXMIPLEVEL: return "MAXMIPLEVEL";
    case D3DSAMP_MAXANISOTROPY: return "MAXANISOTROPY";
    case D3DSAMP_SRGBTEXTURE: return "SRGBTEXTURE";
    case D3DSAMP_ELEMENTINDEX: return "ELEMENTINDEX";
    case D3DSAMP_DMAPOFFSET: return "DMAPOFFSET";
    default: return "?";
  }
}

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-5f * std::fabs(b); }

// True if v is DNEResolution for a w x h viewport: (2/w, -2/h, -1, 1).
bool IsDneResolution(const float* v, float w, float h) {
  return v[2] == -1.0f && v[3] == 1.0f && Near(v[0], 2.0f / w) && Near(v[1], -2.0f / h);
}

// Returns the constants to upload: `data` itself, or a patched copy.
const float* FixConstants(const float* data, UINT count, const char* stage, UINT reg) {
  const D3DVIEWPORT9& vp = t_viewport;
  if (!g_options.fixDneResolution || !data || (vp.X == 0 && vp.Y == 0) || vp.Width == 0 || vp.Height == 0) {
    return data;
  }
  const float w = static_cast<float>(vp.Width), h = static_cast<float>(vp.Height);
  const float* out = data;
  for (UINT i = 0; i < count; ++i) {
    const float* v = data + i * 4;
    if (!IsDneResolution(v, w, h)) continue;
    if (out == data) {
      t_scratch.assign(data, data + count * 4);
      out = t_scratch.data();
    }
    float* p = t_scratch.data() + i * 4;
    p[2] = -1.0f - static_cast<float>(vp.X) * v[0];  // v[0] = 2/w
    p[3] = 1.0f + static_cast<float>(vp.Y) * -v[1];  // v[1] = -2/h
    if (g_fixedCount.fetch_add(1, std::memory_order_relaxed) == 0) {
      VR_LOG_INFO("DNEResolution fix active: %s c%u (%g, %g, -1, 1) -> zw (%g, %g) for viewport %lux%lu at %lu,%lu",
                  stage, reg + i, v[0], v[1], p[2], p[3], vp.Width, vp.Height, vp.X, vp.Y);
    }
  }
  return out;
}

HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9* self, const D3DVIEWPORT9* vp) {
  if (vp) {
    t_viewport = *vp;
    if (t_trace) {
      VR_LOG_INFO("  SetViewport x=%lu y=%lu w=%lu h=%lu z=%.3f..%.3f", vp->X, vp->Y, vp->Width, vp->Height,
                  vp->MinZ, vp->MaxZ);
    }
  }
  return g_origSetViewport(self, vp);
}

HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* texture) {
  if (stage == 0) t_stage0 = texture;
  if (t_trace && texture) {
    D3DSURFACE_DESC desc{};
    if (texture->GetType() == D3DRTYPE_TEXTURE &&
        SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc))) {
      VR_LOG_INFO("  SetTexture s%lu = %p (%ux%u fmt %d usage 0x%lx)", stage, static_cast<void*>(texture), desc.Width,
                  desc.Height, static_cast<int>(desc.Format), desc.Usage);
    } else {
      VR_LOG_INFO("  SetTexture s%lu = %p", stage, static_cast<void*>(texture));
    }
  }
  return g_origSetTexture(self, stage, texture);
}

HRESULT STDMETHODCALLTYPE HookSetSamplerState(IDirect3DDevice9* self, DWORD sampler, D3DSAMPLERSTATETYPE type,
                                              DWORD value) {
  if (t_trace) VR_LOG_INFO("  SetSamplerState s%lu %s = %lu (0x%08lx)", sampler, SamplerStateName(type), value, value);
  return g_origSetSamplerState(self, sampler, type, value);
}

HRESULT STDMETHODCALLTYPE HookSetPixelShader(IDirect3DDevice9* self, IDirect3DPixelShader9* shader) {
  if (t_trace) VR_LOG_INFO("  SetPixelShader %p", static_cast<void*>(shader));
  t_ps = Lookup(shader);
  return g_origSetPixelShader(self, shader);
}

HRESULT STDMETHODCALLTYPE HookSetVertexShader(IDirect3DDevice9* self, IDirect3DVertexShader9* shader) {
  t_vs = Lookup(shader);
  return g_origSetVertexShader(self, shader);
}

HRESULT STDMETHODCALLTYPE HookCreateVertexShader(IDirect3DDevice9* self, const DWORD* code,
                                                 IDirect3DVertexShader9** shader) {
  const HRESULT hr = g_origCreateVertexShader(self, code, shader);
  if (SUCCEEDED(hr) && shader) Remember(*shader, code);
  return hr;
}

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(IDirect3DDevice9* self, const DWORD* code,
                                                IDirect3DPixelShader9** shader) {
  const HRESULT hr = g_origCreatePixelShader(self, code, shader);
  if (SUCCEEDED(hr) && shader) Remember(*shader, code);
  return hr;
}

void TraceConstants(const char* stage, UINT reg, const float* data, UINT count, const float* sent) {
  for (UINT i = 0; i < count; ++i) {
    const float* v = sent + i * 4;
    const bool fixed = sent != data && (v[2] != data[i * 4 + 2] || v[3] != data[i * 4 + 3]);
    VR_LOG_INFO("  Set%sShaderConstantF c%u = (%.9g, %.9g, %.9g, %.9g)%s", stage, reg + i, v[0], v[1], v[2], v[3],
                fixed ? " [DNEResolution fixed]" : "");
  }
}

HRESULT STDMETHODCALLTYPE HookSetVertexShaderConstantF(IDirect3DDevice9* self, UINT reg, const float* data,
                                                       UINT count) {
  const float* sent = FixConstants(data, count, "VS", reg);
  if (t_trace && data) TraceConstants("Vertex", reg, data, count, sent);
  return g_origSetVertexShaderConstantF(self, reg, sent, count);
}

HRESULT STDMETHODCALLTYPE HookSetPixelShaderConstantF(IDirect3DDevice9* self, UINT reg, const float* data,
                                                      UINT count) {
  const float* sent = FixConstants(data, count, "PS", reg);
  if (t_trace && data) TraceConstants("Pixel", reg, data, count, sent);
  return g_origSetPixelShaderConstantF(self, reg, sent, count);
}

}  // namespace

bool Install(IDirect3DDevice9* device, const Options& options) {
  if (g_installed.exchange(true)) return true;
  g_options = options;
  using vrcore::hook::InstallVTable;
  bool ok = InstallVTable("IDirect3DDevice9::SetViewport", device, kSetViewport, &HookSetViewport, &g_origSetViewport);
  ok = ok && InstallVTable("IDirect3DDevice9::SetTexture", device, kSetTexture, &HookSetTexture, &g_origSetTexture);
  ok = ok && InstallVTable("IDirect3DDevice9::SetSamplerState", device, kSetSamplerState, &HookSetSamplerState,
                           &g_origSetSamplerState);
  ok = ok && InstallVTable("IDirect3DDevice9::SetPixelShader", device, kSetPixelShader, &HookSetPixelShader,
                           &g_origSetPixelShader);
  ok = ok && InstallVTable("IDirect3DDevice9::CreateVertexShader", device, kCreateVertexShader,
                           &HookCreateVertexShader, &g_origCreateVertexShader);
  ok = ok && InstallVTable("IDirect3DDevice9::SetVertexShader", device, kSetVertexShader, &HookSetVertexShader,
                           &g_origSetVertexShader);
  ok = ok && InstallVTable("IDirect3DDevice9::CreatePixelShader", device, kCreatePixelShader,
                           &HookCreatePixelShader, &g_origCreatePixelShader);
  ok = ok && InstallVTable("IDirect3DDevice9::SetVertexShaderConstantF", device, kSetVertexShaderConstantF,
                           &HookSetVertexShaderConstantF, &g_origSetVertexShaderConstantF);
  ok = ok && InstallVTable("IDirect3DDevice9::SetPixelShaderConstantF", device, kSetPixelShaderConstantF,
                           &HookSetPixelShaderConstantF, &g_origSetPixelShaderConstantF);
  VR_LOG_INFO("device state hooks %s; DNEResolution fix %s", ok ? "installed" : "FAILED",
              g_options.fixDneResolution ? "on" : "off");
  return ok;
}

void SetTrace(bool enabled) { t_trace = enabled; }

const D3DVIEWPORT9& CurrentViewport() { return t_viewport; }

bool Stage0TextureSize(UINT& width, UINT& height) {
  if (!t_stage0 || t_stage0->GetType() != D3DRTYPE_TEXTURE) return false;
  D3DSURFACE_DESC desc{};
  if (FAILED(static_cast<IDirect3DTexture9*>(t_stage0)->GetLevelDesc(0, &desc))) return false;
  width = desc.Width;
  height = desc.Height;
  return true;
}
int VertexShaderConstant(const char* name) { return FindConstant(t_vs, name); }
int PixelShaderConstant(const char* name) { return FindConstant(t_ps, name); }

}  // namespace rm::device_state
