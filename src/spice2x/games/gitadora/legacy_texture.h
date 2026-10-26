#pragma once

#include <windows.h>

namespace games::gitadora::legacy::texture {
    // XG1 JP's packed luminance/alpha textures use a format unsupported by
    // current drivers. Keep the native texture lifecycle and expand the input.
    bool attach(HMODULE tools);
}
