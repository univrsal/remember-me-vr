#pragma once

#include <cstddef>

namespace vrcore::hook {

bool Init();
void Shutdown();

// Creates and enables an inline hook on `target`. `original` receives the
// trampoline used to call the unhooked function. Returns false on failure
// (and logs why).
bool Install(const char* name, void* target, void* detour, void** original);

template <typename Fn>
bool Install(const char* name, Fn target, Fn detour, Fn* original) {
  return Install(name, reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour),
                 reinterpret_cast<void**>(original));
}

// Hooks the function in slot `index` of a COM object's vtable. All objects of
// the same implementation class share the vtable, so this hooks the method for
// every instance.
template <typename Fn>
bool InstallVTable(const char* name, void* comObject, size_t index, Fn detour, Fn* original) {
  void** vtable = *reinterpret_cast<void***>(comObject);
  return Install(name, vtable[index], reinterpret_cast<void*>(detour),
                 reinterpret_cast<void**>(original));
}

}  // namespace vrcore::hook
