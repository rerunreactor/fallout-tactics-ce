# SDL2, fetched at configure time and built as a static library.
#
# The pin and the first three options are the ones fallout2-ce's
# third_party/sdl2 uses, so the two engines build SDL2 the same way on Windows
# and macOS; the last two only keep SDL2 out of our install and our build
# log. Linux CI uses the distribution's libsdl2-dev instead
# (FOT_VENDORED=OFF), as fallout2-ce's Linux presets do.

# Build the static library only.
set(SDL_SHARED OFF)
set(SDL_STATIC ON)

# Fixes "_ftol2_sse already defined" with MSVC.
set(SDL_LIBC ON)

# Our install() rules ship the four executables, not SDL2's headers and
# libraries.
set(SDL2_DISABLE_INSTALL ON)

# No compiler launcher behind the build's back. (SDL's ccache block also asks
# for cmake_minimum_required 3.4, which CMake 4 warns about.)
set(SDL_CCACHE OFF)

include(FetchContent)

FetchContent_Declare(sdl2
    GIT_REPOSITORY "https://github.com/libsdl-org/SDL"
    GIT_TAG "release-2.32.8"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    ${FOT_FETCH_EXCLUDE_FROM_ALL}
)

FetchContent_MakeAvailable(sdl2)

if(CMAKE_VERSION VERSION_LESS 3.28)
    # Older CMake has no EXCLUDE_FROM_ALL in FetchContent_Declare.
    set_property(DIRECTORY "${sdl2_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
endif()
