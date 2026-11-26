#include "vrcore/hook.h"

#include <MinHook.h>

#include "vrcore/log.h"

namespace vrcore::hook {

bool Init() {
  const MH_STATUS status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
    VR_LOG_ERROR("MinHook init failed: %s", MH_StatusToString(status));
    return false;
  }
  return true;
}

void Shutdown() {
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();
}

bool Install(const char* name, void* target, void* detour, void** original) {
  MH_STATUS status = MH_CreateHook(target, detour, original);
  if (status == MH_OK) status = MH_EnableHook(target);
  if (status != MH_OK) {
    VR_LOG_ERROR("hook %s @ %p failed: %s", name, target, MH_StatusToString(status));
    return false;
  }
  VR_LOG_DEBUG("hooked %s @ %p", name, target);
  return true;
}

}  // namespace vrcore::hook
