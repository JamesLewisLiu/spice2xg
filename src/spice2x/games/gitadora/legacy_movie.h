#pragma once

#include <windows.h>

namespace games::gitadora::legacy::movie {
// Initialize movie diagnostics without reserving address space. Production
// uses on-demand pages for large buffers, with paired native CRT fallback.
void prepare();
// Validate both modules and install all six callsites as one transaction.
bool attach(HMODULE module, HMODULE game);
} // namespace games::gitadora::legacy::movie
