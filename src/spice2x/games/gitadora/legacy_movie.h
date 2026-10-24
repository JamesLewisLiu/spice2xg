#pragma once

#include <windows.h>

namespace games::gitadora::legacy::movie {
// Reserve before loading the native dependency graph. The native source
// filters can outlive launcher cleanup, so reservations last until process
// exit.
void prepare();
// Validate both modules and install all five callsites as one transaction.
bool attach(HMODULE module, HMODULE game);
} // namespace games::gitadora::legacy::movie
