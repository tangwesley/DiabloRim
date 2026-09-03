cmake_minimum_required(VERSION 3.21)

# --- Force-disable CommonLibSSE-NG test/tooling options (prevent Catch2/regex conflicts) ---
option(CommonLibSSE_POSH_GENERATION "Generate .posh.d.ts type definition file" OFF)
option(CommonLibSSE_TESTING "Build tests" OFF)
set(CommonLibSSE_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ★A STATIC CRT (/MT), and the triplet in CMakePresets.json must agree
# (x64-windows-static). Same pairing Grid Inventory uses, and the ABI header is
# what makes it safe: no STL type and no pointer either side has to free ever
# crosses the plugin boundary, so the two DLLs having separate CRT heaps costs
# nothing. Change one of these two and you must change the other, or the link
# fails on mismatched runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(DiabloInSkyrim VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Packages (order matters) ---
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# CommonLibSSE-NG expects RAPIDCSV_INCLUDE_DIRS (header-only rapidcsv from vcpkg).
# We will also parse the affix table with it, so it earns its place twice.
find_path(RAPIDCSV_INCLUDE_DIRS "rapidcsv.h")

# --- CommonLibSSE-NG from source -----------------------------------------
# ★PINNED, AND NOT TO UPSTREAM. This is the same fork and the same commit Grid
# Inventory uses, and the reason is worth repeating rather than rediscovering:
#
# CharmedBaryon's tree decides the runtime from the MINOR version with
#
#     case 4: VR    case 6: AE    default: SE
#
# so 1.7.99 classifies as SE. Two consequences, both fatal: the address library
# is looked for under the SE name (version-1-7-99-0.bin instead of
# versionlib-...), which is the "failed to open address library file" every
# 1.7.99 player reports, and every RELOCATION_ID(se, ae) resolves to the SE id —
# the wrong address for every call. alandtse's NG line has
#
#     if (v[1] == 4) VR; else if (v[1] >= 6) AE; else SE;
#
# and reads the address library's FORMAT from the file rather than assuming it
# from the runtime, which is what Address Library v12 needs.
#
# ★A COMMIT, not the `ng` branch. A moving branch puts unreviewed code into the
# next build of a plugin whose failure mode is a crash on someone else's
# machine. Bump it deliberately, and in step with Grid Inventory.
include(FetchContent)
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/alandtse/CommonLibVR.git
    GIT_TAG 0e167dd4c3981d586a53953e2c1a608e2c4ccedf   # ng @ 2026-08-21
)
FetchContent_MakeAvailable(CommonLibSSE)

# --- The roller: pure logic, no Skyrim -------------------------------------
# ★A SEPARATE STATIC LIBRARY, and that is the whole point. It links against
# nothing -- not CommonLibSSE, not spdlog, not the PCH -- so the balance harness
# below can link the exact same code and run outside the game. Loot balance is
# found by iterating, and an iteration that costs a Skyrim launch will not get
# done often enough to be any good.
#
# If anything in src/roll/ ever needs RE/ or SKSE/, that is the signal it belongs
# in the plugin instead.
add_library(DiabloRoll STATIC
    src/roll/Roll.h
    src/roll/AffixTable.cpp
    src/roll/Roller.cpp
)
target_compile_features(DiabloRoll PUBLIC cxx_std_23)
target_compile_options(DiabloRoll PRIVATE /utf-8)
target_include_directories(DiabloRoll PUBLIC src)

# --- Balance harness: rolls a sample and prints the distribution -------------
add_executable(RollHarness tools/RollHarness/main.cpp)
target_link_libraries(RollHarness PRIVATE DiabloRoll)
target_compile_options(RollHarness PRIVATE /utf-8)

# --- Plugin target ---
set(SOURCES
    src/PCH.h
    src/main.cpp
    # ★THROWAWAY. Phase 0 only -- delete these four files, and their call sites
    # in main.cpp, once the five spike questions there have answers.
    src/MgefSurvey.h
    src/MgefSurvey.cpp
)

# Phase 4: the boundary between the pure roller and the engine.
list(APPEND SOURCES src/Apply.h src/Apply.cpp)
list(APPEND SOURCES src/Config.h src/Config.cpp)

# Phase 5: distribution. Persistence first -- it is the part that can damage a
# save rather than merely misbehave.
list(APPEND SOURCES src/Persist.h src/Persist.cpp)
list(APPEND SOURCES src/Distribute.h src/Distribute.cpp)

# Phase 5b: the player's own inventory, which Distribute refuses on purpose --
# the one place a quest reward actually lands.
list(APPEND SOURCES src/QuestReward.h src/QuestReward.cpp)

# The one vtable hook in the project: swallowing the HUD notification a reward
# fires before its roll has landed. Notify.h argues why this hook and not others.
list(APPEND SOURCES src/Notify.h src/Notify.cpp)

# The arcane enchanter: affixed items are offered to the table as unenchanted,
# and the player's enchantment is merged back onto them on the way out.
list(APPEND SOURCES src/Enchanting.h src/Enchanting.cpp)

# Grid Inventory tint: the ABI shim that lets another mod's UI colour our items.
# api/GridInventoryAPI.h is a VENDORED COPY of the host's contract header, not a
# reference into its source tree -- the two mods ship separately, and a build
# that reached across repositories would break the moment either one moved.
list(APPEND SOURCES src/api/GridInventoryAPI.h)
list(APPEND SOURCES src/GridTint.h src/GridTint.cpp)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
# ★NOMINMAX as a COMPILE DEFINITION, not a #define in the PCH. CommonLibSSE-NG
# pulls in Windows.h itself, so by the time our own header runs the min/max
# macros already exist. Defining it on the command line is the only placement
# that beats every include.
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /Zi)   # UTF-8 string literals + PDB
# PDB for crash-log symbolication; keep release layout (/OPT defaults flip with /DEBUG)
target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE src ${RAPIDCSV_INCLUDE_DIRS})

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    DiabloRoll
    fmt::fmt
    spdlog::spdlog
)

# --- Deploy DLL to a local MO2 mod folder (developer convenience) ---
# Point MOD_ROOT at your own mod folder (-DMOD_ROOT=...). The copy step is
# skipped entirely when the folder does not exist, so a plain clone builds
# without any local setup.
set(MOD_ROOT "H:/Game/Fablerim/mods/Diablo In Skyrim" CACHE PATH
    "MO2 mod folder the built DLL is copied into (skipped if absent)")
if(EXISTS "${MOD_ROOT}")
    set(DLL_DEPLOY "${MOD_ROOT}/SKSE/Plugins")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DLL_DEPLOY}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_DEPLOY}/"
        VERBATIM
    )
    # The affix table ships beside the DLL: users edit it to rebalance without
    # a rebuild, which is the entire point of it being a CSV.
    #
    # ★ITS OWN ALWAYS-RUN TARGET, not a POST_BUILD on the plugin. POST_BUILD only
    # fires when the DLL actually relinks, so a build that changed nothing but the
    # CSV left the old table deployed -- measured, and exactly the failure that is
    # hardest to notice, because the build succeeds and the game quietly keeps
    # reading yesterday's data.
    add_custom_target(DeployAffixTable ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/data/DiabloLoot_affixes.csv" "${DLL_DEPLOY}/"
        # ★NEVER OVERWRITE AN EXISTING INI. copy_if_different compares CONTENT,
        # so it happily replaces a file the user has edited -- which it did,
        # silently reverting Discovery=1 between a build and the next launch and
        # costing a wasted run. Config is the user\x27s; data is ours. Only place
        # the default when there is nothing there.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/data/DiabloLoot.ini" "${DLL_DEPLOY}/DiabloLoot.ini.default"
        DEPENDS "${CMAKE_SOURCE_DIR}/data/DiabloLoot_affixes.csv"
        COMMENT "Deploying affix table"
        VERBATIM
    )
    message(STATUS "DiabloInSkyrim: deploying to ${DLL_DEPLOY}")
else()
    message(STATUS "DiabloInSkyrim: MOD_ROOT not found, skipping deploy (${MOD_ROOT})")
endif()
