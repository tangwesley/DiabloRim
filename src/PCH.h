#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

// NOMINMAX lives in CMakeLists as a compile definition, not here: CommonLibSSE-NG
// includes Windows.h before this file is reached, so a #define at this point is
// already too late to stop windef.h claiming min and max.

// Windows.h AFTER the CommonLibSSE-NG headers, or its macros clash with REX.
#include <Windows.h>

// ★wingdi.h defines GetObject as GetObjectW/GetObjectA, which silently renames
// any member of that name -- RE::BGSDefaultObjectManager::GetObject comes back
// as "'GetObjectA' is not a member of". Parentheses do not help (it is an
// OBJECT-like macro), and the error names a symbol that appears nowhere in our
// source, so it reads as a CommonLibSSE fault rather than a macro one. Dropped
// here, once, beside the include that causes it. Nothing here calls the GDI
// function. (Same fix, same reason, as Grid Inventory's PCH.)
#ifdef GetObject
#    undef GetObject
#endif

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

using namespace std::literals;

namespace logger = SKSE::log;
