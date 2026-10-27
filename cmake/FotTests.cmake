# FotTests.cmake -- the CTest tests, included from the top-level CMakeLists.txt.
#
# smoke.*  Run each program with no input and expect its usage text. They need
#          nothing, so CI runs them on every platform; they prove the binaries
#          start (static SDL2 on Windows and macOS, the system SDL2 on Linux).
#
# data.*   Need the player's own copy of Fallout Tactics: point the environment
#          variable FOT_DATA_DIR at the game's `core` folder, the one holding
#          spr-sprites_0.bos, tiles_0.bos and the rest. Without it they report
#          themselves as skipped. FOT_DATA_DIR is only ever read. Everything
#          the tests write -- the archives sprview and mapview need unpacked
#          (they read loose files, not .bos), renders, dumps, saves -- goes
#          under <build>/data-tests/, which data.cleanup deletes at the end.
#          Set FOT_DATA_KEEP=1 to keep it for a look.
#
#   FOT_DATA_DIR="/path/to/Fallout Tactics/core" ctest --test-dir build -j4 -L data
#
# The checks are the reimplementation's old milestone ones, widened from
# "spr-sprites_0.bos plus the critters" to every sprite archive and from
# "85+ maps" to every map the archives and the loose tree hold.

set(FOT_DATA_WORK "${PROJECT_BINARY_DIR}/data-tests")
set(FOT_DATA_DRIVER "${CMAKE_CURRENT_LIST_DIR}/FotDataTest.cmake")

# --- smoke ---------------------------------------------------------------------
add_test(NAME smoke.fotgame COMMAND fotgame)
add_test(NAME smoke.mapview COMMAND mapview)
add_test(NAME smoke.sprview COMMAND sprview)
add_test(NAME smoke.sprtrim COMMAND sprtrim --help)
set_tests_properties(smoke.fotgame smoke.mapview smoke.sprview smoke.sprtrim PROPERTIES
    PASS_REGULAR_EXPRESSION "usage:"
    LABELS smoke
    TIMEOUT 60
)

# --- data ----------------------------------------------------------------------
# fot_data_test(<name> <mode> [<extra -D arguments>...])
function(fot_data_test name mode)
    add_test(NAME data.${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DMODE=${mode}"
            "-DWORK=${FOT_DATA_WORK}"
            "-DFOTGAME=$<TARGET_FILE:fotgame>"
            "-DMAPVIEW=$<TARGET_FILE:mapview>"
            "-DSPRVIEW=$<TARGET_FILE:sprview>"
            "-DSPRTRIM=$<TARGET_FILE:sprtrim>"
            ${ARGN}
            -P "${FOT_DATA_DRIVER}"
    )
    set_tests_properties(data.${name} PROPERTIES
        SKIP_REGULAR_EXPRESSION "FOT_DATA_TEST_SKIPPED"
        LABELS data
        TIMEOUT 1800
    )
    # fot_data: the unpacked archives, which sprview, sprtrim and mapview
    # need. fot_work: <build>/data-tests itself, which every data test
    # writes to and data.cleanup removes once they have all finished.
    if(mode STREQUAL "unpack")
        set_tests_properties(data.${name} PROPERTIES FIXTURES_SETUP fot_data)
    elseif(mode STREQUAL "cleanup")
        set_tests_properties(data.${name} PROPERTIES FIXTURES_CLEANUP "fot_data;fot_work")
    elseif(mode MATCHES "^fotgame-")
        # fotgame reads the archives in place and needs nothing unpacked.
        set_tests_properties(data.${name} PROPERTIES FIXTURES_REQUIRED fot_work)
    else()
        set_tests_properties(data.${name} PROPERTIES FIXTURES_REQUIRED "fot_data;fot_work")
    endif()
endfunction()

fot_data_test(unpack unpack)

# sprview: every sprite of every spr-*.bos decodes, 0 errors, every layer
# inside its frame rect; and --info/--dump/--composite on a sample.
fot_data_test(sprview.selftest sprview-selftest)
fot_data_test(sprview.dump sprview-dump)

# sprtrim: every sprite repacks and trims, and both reload identically.
fot_data_test(sprtrim.selftest sprtrim-selftest)

# mapview: every map parses (85+, 0 parse errors, every tile prototype
# resolved) and renders. The renders are split four ways so `ctest -j`
# can run them side by side.
fot_data_test(mapview.selftest mapview-selftest)
set(FOT_RENDER_SHARDS 4)
foreach(shard RANGE 1 ${FOT_RENDER_SHARDS})
    fot_data_test(mapview.render.${shard} mapview-render "-DSHARD=${shard}" "-DSHARDS=${FOT_RENDER_SHARDS}")
endforeach()

# fotgame reads the .bos archives in place, as the browser build does.
fot_data_test(fotgame.weapons fotgame-weapons)
fot_data_test(fotgame.mission fotgame-mission)
fot_data_test(fotgame.campaign fotgame-campaign)

fot_data_test(cleanup cleanup)
