# Fallout Tactics: Community Edition -- the fotgame engine and its tools.
#
# Native (fotgame, mapview, sprview, sprtrim; on macOS also Fallout Tactics CE.app):
#   cmake -S . -B build
#   cmake --build build --parallel
#   cmake --install build --prefix <dir>   -> the layout of a release zip
#
# Web (fotgame only, the build the RerunReactor shell loads):
#   source <emsdk>/emsdk_env.sh
#   emcmake cmake -S . -B build-web
#   cmake --build build-web --parallel
#   -> build-web/bin/fotgame.js.js + build-web/bin/fotgame.js.wasm
#   cmake --install build-web --prefix <shell>/engine-dist
#   -> copies just those two files there (what `make install-wasm` did)
#
# AddressSanitizer + UndefinedBehaviorSanitizer (the old game/Makefile's
# `asan` target):
#   cmake -S . -B build-asan -DFOT_SANITIZE=ON
#
# Tests. The data tests need the player's own game data and skip without it:
#   FOT_DATA_DIR=/path/to/Fallout\ Tactics/core ctest --test-dir build

cmake_minimum_required(VERSION 3.22...3.31)

# --- Defaults that project() reads ------------------------------------------
#
# The old Makefiles compiled everything, native and Emscripten alike, with
# plain -O2 and without -DNDEBUG. Seed the Release flags with exactly that.
# These are cache defaults, so a value given on the command line still wins.
# (cl.exe accepts -O2 as /O2, which implies /Ob2.)
set(CMAKE_C_FLAGS_RELEASE "-O2" CACHE STRING "Flags used by the C compiler during Release builds.")
set(CMAKE_CXX_FLAGS_RELEASE "-O2" CACHE STRING "Flags used by the C++ compiler during Release builds.")

# std::filesystem (common/vfs.cpp, mapview/) needs macOS 10.15.
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version to target.")
endif()

# The version stamped into the binaries: the Windows version information and
# the Mac app's Info.plist. A release build takes it from its tag (CI passes
# -DFOT_VERSION=0.1.1 for v0.1.1); otherwise this default applies, which
# should be the latest release.
set(FOT_VERSION "0.1.1" CACHE STRING "Version stamped into the binaries (major.minor.patch)")

project(fallout-tactics-ce VERSION ${FOT_VERSION} LANGUAGES C CXX)

# Keep the builder's absolute paths out of the binaries: __FILE__ (in asserts,
# including stb_image's) and debug info see paths relative to the source and
# build trees instead. Set before the dependencies so fetched SDL2 gets it too.
if(NOT MSVC)
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/="
                        "-ffile-prefix-map=${CMAKE_BINARY_DIR}/=")
endif()

get_property(FOT_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT FOT_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: Release, Debug, RelWithDebInfo or MinSizeRel." FORCE)
endif()

option(FOT_VENDORED "Fetch and build SDL2 and zlib (as fallout2-ce does) instead of using installed copies" ON)
option(FOT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(FOT_TESTS "Register the CTest tests; the data tests skip unless FOT_DATA_DIR is set" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A static CRT on Windows, so the release zips need no VC++ redistributable.
# The policy defaults make the fetched SDL2 (whose cmake_minimum_required
# predates these policies) follow this setting and the option() values the
# third_party snippets set, instead of its own defaults.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

include(GNUInstallDirs)

# --- Dependencies: SDL2 and zlib ----------------------------------------------

if(EMSCRIPTEN)
    if(FOT_SANITIZE)
        message(FATAL_ERROR "FOT_SANITIZE is for native GCC/Clang builds, not the web build.")
    endif()

    # The old game/Makefile's EMFLAGS, on every object and on the link:
    # Emscripten's own SDL2 and zlib ports, and a heap that can grow (the
    # shell writes the player's archives into MEMFS at boot). -std=c++17 comes
    # from CMAKE_CXX_STANDARD and -O2 from CMAKE_CXX_FLAGS_RELEASE above, which
    # CMake also passes to the link.
    set(FOT_WEB_FLAGS -sUSE_SDL=2 -sUSE_ZLIB=1 -sALLOW_MEMORY_GROWTH=1)
    add_compile_options(${FOT_WEB_FLAGS})
    add_link_options(${FOT_WEB_FLAGS})
elseif(FOT_VENDORED)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        set(FOT_FETCH_EXCLUDE_FROM_ALL EXCLUDE_FROM_ALL)
    endif()
    add_subdirectory(third_party/zlib)
    add_subdirectory(third_party/sdl2)
    set(FOT_ZLIB_TARGET zlibstatic)
    set(FOT_SDL2_TARGET SDL2::SDL2-static)
    set(FOT_SDL2MAIN_TARGET SDL2::SDL2main)
else()
    find_package(ZLIB REQUIRED)
    find_package(SDL2 REQUIRED)
    set(FOT_ZLIB_TARGET ZLIB::ZLIB)
    if(TARGET SDL2::SDL2)
        set(FOT_SDL2_TARGET SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        set(FOT_SDL2_TARGET SDL2::SDL2-static)
    else()
        # An old sdl2-config.cmake that only sets variables.
        add_library(fot_sdl2 INTERFACE)
        target_include_directories(fot_sdl2 INTERFACE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(fot_sdl2 INTERFACE ${SDL2_LIBRARIES})
        set(FOT_SDL2_TARGET fot_sdl2)
    endif()
    if(TARGET SDL2::SDL2main)
        set(FOT_SDL2MAIN_TARGET SDL2::SDL2main)
    endif()
endif()

# --- Settings every target of ours shares ---------------------------------------

function(fot_configure_target target)
    if(NOT EMSCRIPTEN)
        # The web build keeps the old game/Makefile's exact compile line,
        # which had no warning flags.
        if(MSVC)
            target_compile_options(${target} PRIVATE /W3 /utf-8 /bigobj)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra)
        endif()
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNINGS
            _USE_MATH_DEFINES
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()
    if(FOT_SANITIZE)
        if(MSVC)
            message(FATAL_ERROR "FOT_SANITIZE supports GCC and Clang only.")
        endif()
        # The old game/Makefile's ASAN flags. -O1 comes after the build
        # type's -O2, so it is the one that applies.
        set(sanitize -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
        target_compile_options(${target} PRIVATE ${sanitize})
        target_link_options(${target} PRIVATE ${sanitize})
    endif()
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    endif()
endfunction()

# --- fot_shared: the code the engine and the tools have in common ---------------
#
# Each old Makefile recompiled its own copy of these. A static library also
# keeps the tools exactly as lean as before: the linker only takes the objects
# a program uses, so sprview and sprtrim still get no stb_image or minimp3.
add_library(fot_shared STATIC
    common/file.cpp
    common/image.cpp
    common/mp3.cpp
    common/png.cpp
    common/vfs.cpp
    common/zar.cpp
    sprview/spr.cpp
    mapview/til.cpp
    mapview/mis.cpp
    mapview/esh.cpp
    mapview/ent.cpp
    mapview/light.cpp
    engine/shared.cpp
)
target_include_directories(fot_shared PUBLIC common sprview mapview engine)
if(NOT EMSCRIPTEN)
    target_link_libraries(fot_shared PUBLIC ${FOT_ZLIB_TARGET})
endif()
fot_configure_target(fot_shared)

# --- fotgame: the engine ---------------------------------------------------------
# WIN32: on Windows a GUI program, so a double-click opens no console window
# (launch.cpp reattaches the console when it's started from one).
add_executable(fotgame WIN32
    game/main.cpp
    game/launch.cpp
    game/scene.cpp
    game/move.cpp
    game/combat.cpp
    game/script.cpp
    game/ai.cpp
    game/ui.cpp
    game/ui_screens.cpp
    game/campaign.cpp
    game/campaign_save.cpp
    game/campaign_ui.cpp
    game/audio.cpp
    game/audio_names.cpp
)
target_include_directories(fotgame PRIVATE game)
target_link_libraries(fotgame PRIVATE fot_shared)
fot_configure_target(fotgame)

# The folder picker a double-clicked fotgame shows (game/launch.h), and the
# Windows icon and version information.
if(WIN32)
    set(FOT_WINDOWS_ICON "${PROJECT_SOURCE_DIR}/packaging/windows/fotgame.ico")
    configure_file(packaging/windows/fotgame.rc.in "${PROJECT_BINARY_DIR}/fotgame.rc" @ONLY)
    target_sources(fotgame PRIVATE "${PROJECT_BINARY_DIR}/fotgame.rc")
    target_link_libraries(fotgame PRIVATE ole32 uuid)
elseif(APPLE)
    # Objective-C++, compiled by the C++ compiler. Enabling CMake's OBJCXX
    # language instead would also claim the fetched SDL2's .m files.
    set_source_files_properties(game/folder_picker_mac.mm PROPERTIES
        LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++")
    target_sources(fotgame PRIVATE game/folder_picker_mac.mm)
    target_link_libraries(fotgame PRIVATE "-framework AppKit")
endif()

if(EMSCRIPTEN)
    # The rest of the old game/Makefile's EMLINK, unchanged; the RerunReactor
    # shell (shell/src/engine.ts) depends on all of it. There is no
    # --preload-file: the shell mounts the player's own core/*.bos into MEMFS
    # and common/vfs.cpp reads them in place, so the engine ships as just the
    # .js and the .wasm.
    #   MODULARIZE, EXPORT_NAME  the shell loads the script, then calls
    #                            window.falloutTacticsCe(cfg) as a factory: the
    #                            contract of the fallout1-ce and fallout2-ce
    #                            builds too.
    #   FORCE_FILESYSTEM         nothing is preloaded, so without it the FS
    #                            would be optimised out and Module.FS undefined.
    #   EXPORTED_RUNTIME_METHODS FS for the mount, callMain to start; ccall and
    #                            UTF8ToString read rr_campaign_state().
    #   EXPORTED_FUNCTIONS       main, and rr_campaign_state for the shell.
    #   -lidbfs.js               saves persist in IndexedDB.
    #   MAXIMUM_MEMORY=4GB       wasm32's ceiling; the retail core is 1.5 GB of
    #                            archives, plus decode caches.
    #   INVOKE_RUN=0             the shell calls main once the FS is populated.
    #   EXIT_RUNTIME=0           returning from main must not tear the FS down
    #                            before the last syncfs lands.
    # No pthreads: the renderer is single-threaded, and a pthreads build would
    # need COOP/COEP headers on the whole site.
    target_link_options(fotgame PRIVATE
        -sMODULARIZE=1
        -sEXPORT_NAME=falloutTacticsCe
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=callMain,FS,ENV,ccall,UTF8ToString
        -sEXPORTED_FUNCTIONS=_main,_rr_campaign_state
        -lidbfs.js
        -sMAXIMUM_MEMORY=4GB
        -sINVOKE_RUN=0
        -sEXIT_RUNTIME=0
        -sSTACK_SIZE=5MB
    )
    # Deliberately `fotgame.js.js`: emcc names the sidecar after the output,
    # so this makes the loader fetch `fotgame.js.wasm`, which is the name the
    # shell vendors it under, next to fallout1-ce.js.js and fallout2-ce.js.js.
    set_target_properties(fotgame PROPERTIES OUTPUT_NAME "fotgame.js")
    # Installed flat, so that `cmake --install build-web --prefix
    # <shell>/engine-dist` vendors the engine where the shell looks for it.
    install(TARGETS fotgame RUNTIME DESTINATION .)
    install(FILES "$<TARGET_FILE_DIR:fotgame>/fotgame.js.wasm" DESTINATION .)
else()
    target_link_libraries(fotgame PRIVATE ${FOT_SDL2_TARGET})
    if(WIN32 AND FOT_SDL2MAIN_TARGET)
        # SDL.h renames main() to SDL_main() on Windows; SDL2main supplies the
        # real entry point.
        target_link_libraries(fotgame PRIVATE ${FOT_SDL2MAIN_TARGET})
    endif()

    # --- The tools (native only) -----------------------------------------------
    add_executable(mapview mapview/main.cpp)
    target_link_libraries(mapview PRIVATE fot_shared)
    fot_configure_target(mapview)

    add_executable(sprview sprview/main.cpp)
    target_link_libraries(sprview PRIVATE fot_shared)
    fot_configure_target(sprview)

    add_executable(sprtrim sprtrim/main.cpp sprtrim/trim.cpp)
    target_link_libraries(sprtrim PRIVATE fot_shared)
    fot_configure_target(sprtrim)

    # --- The Mac app -------------------------------------------------------------
    # "Fallout Tactics CE.app" next to fotgame in <build>/bin, refreshed after
    # every link: the same binary, an Info.plist and the icon. Signing it ad hoc
    # seals the bundle; it isn't a Developer ID signature, so a downloaded copy
    # still has to be allowed once in System Settings.
    if(APPLE)
        set(FOT_APP "${PROJECT_BINARY_DIR}/bin/Fallout Tactics CE.app")
        configure_file(packaging/macos/Info.plist.in "${PROJECT_BINARY_DIR}/Info.plist" @ONLY)
        add_custom_command(TARGET fotgame POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${FOT_APP}/Contents/MacOS" "${FOT_APP}/Contents/Resources"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "$<TARGET_FILE:fotgame>" "${FOT_APP}/Contents/MacOS/fotgame"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "${PROJECT_BINARY_DIR}/Info.plist" "${FOT_APP}/Contents/Info.plist"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "${PROJECT_SOURCE_DIR}/packaging/macos/AppIcon.icns"
                    "${FOT_APP}/Contents/Resources/AppIcon.icns"
            COMMAND codesign --force --sign - --timestamp=none "${FOT_APP}"
            VERBATIM)
    endif()

    # --- What the release zips hold ------------------------------------------------
    # The game where a player looks for it, at the top: fotgame.exe on Windows,
    # the app on macOS. The modding tools go in tools/. Linux keeps bin/.
    if(WIN32)
        install(TARGETS fotgame RUNTIME DESTINATION .)
        install(TARGETS mapview sprview sprtrim RUNTIME DESTINATION tools)
        install(FILES packaging/windows/how-to-play.txt DESTINATION . RENAME "How to play.txt")
    elseif(APPLE)
        install(DIRECTORY "${FOT_APP}" DESTINATION . USE_SOURCE_PERMISSIONS)
        install(TARGETS mapview sprview sprtrim RUNTIME DESTINATION tools)
        install(FILES packaging/macos/how-to-play.txt DESTINATION . RENAME "How to play.txt")
    else()
        install(TARGETS fotgame mapview sprview sprtrim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES packaging/linux/how-to-play.txt DESTINATION . RENAME "How to play.txt")
    endif()

    # --- Tests ------------------------------------------------------------------
    if(FOT_TESTS)
        enable_testing()
        include(cmake/FotTests.cmake)
    endif()
endif()
