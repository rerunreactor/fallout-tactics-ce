# zlib, fetched at configure time and built as a static library.
#
# The pin is the one fallout2-ce's third_party/zlib uses.

# Do not build the examples, and do not install zlib alongside our programs.
set(ZLIB_BUILD_EXAMPLES OFF)
set(SKIP_INSTALL_ALL ON)

include(FetchContent)

FetchContent_Declare(zlib
    GIT_REPOSITORY "https://github.com/madler/zlib"
    GIT_TAG "v1.3.1"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    ${FOT_FETCH_EXCLUDE_FROM_ALL}
)

FetchContent_MakeAvailable(zlib)

if(CMAKE_VERSION VERSION_LESS 3.28)
    set_property(DIRECTORY "${zlib_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
endif()

# zlib's own CMakeLists gives `zlibstatic` no usage requirements: zlib.h is in
# the source tree and the generated zconf.h in the build tree. Attach both, so
# that linking the target is all a consumer has to do.
target_include_directories(zlibstatic INTERFACE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
