#pragma once

#include <windows.h>

namespace vrcore::detail {

void Attach(HMODULE self);
void Detach();

bool InstallD3D9Hooks();

}  // namespace vrcore::detail
