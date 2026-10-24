#pragma once

#include <windows.h>

namespace games::gitadora::legacy::codec {
// Install after the launcher's global audio COM sweep, before native boot.
void attach(HMODULE movie);
} // namespace games::gitadora::legacy::codec
