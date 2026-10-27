# FotDataTest.cmake -- runs one data test. FotTests.cmake registers them:
#
#   cmake -DMODE=<mode> -DWORK=<dir> -DFOTGAME=<exe> -DMAPVIEW=<exe>
#         -DSPRVIEW=<exe> -DSPRTRIM=<exe> [-DSHARD=<i> -DSHARDS=<n>]
#         -P FotDataTest.cmake
#
# The environment variable FOT_DATA_DIR names the game's `core` folder and is
# only ever read. Everything written goes under WORK. A test that cannot run
# prints FOT_DATA_TEST_SKIPPED, which CTest reports as a skip.

cmake_minimum_required(VERSION 3.22)

set(unpacked "${WORK}/unpacked")

macro(fot_skip why)
    message("FOT_DATA_TEST_SKIPPED: ${why}")
    return()
endmacro()

# --- cleanup (the fixture's last step) ------------------------------------------
if(MODE STREQUAL "cleanup")
    if("$ENV{FOT_DATA_KEEP}" STREQUAL "" OR "$ENV{FOT_DATA_KEEP}" STREQUAL "0")
        file(REMOVE_RECURSE "${WORK}")
    else()
        message("FOT_DATA_KEEP is set: leaving ${WORK} in place")
    endif()
    if("$ENV{FOT_DATA_DIR}" STREQUAL "")
        fot_skip("FOT_DATA_DIR is not set")
    endif()
    return()
endif()

# --- Is there game data? -----------------------------------------------------------
set(data "$ENV{FOT_DATA_DIR}")
if(data STREQUAL "")
    fot_skip("FOT_DATA_DIR is not set; point it at Fallout Tactics' core folder to run the data tests")
endif()
file(TO_CMAKE_PATH "${data}" data)
if(NOT IS_DIRECTORY "${data}")
    fot_skip("FOT_DATA_DIR=${data} is not a directory")
endif()

# Match names case-insensitively: a copy taken from a Windows install keeps
# its own casing (Mis-Main_0.bos).
file(GLOB entries LIST_DIRECTORIES true RELATIVE "${data}" "${data}/*")
set(archives "")
set(archives_lower "")
set(loose_map_roots "")
foreach(entry IN LISTS entries)
    string(TOLOWER "${entry}" lower)
    if(lower MATCHES "\\.bos$" AND NOT IS_DIRECTORY "${data}/${entry}")
        list(APPEND archives "${entry}")
        list(APPEND archives_lower "${lower}")
    elseif(lower MATCHES "^(campaigns|missions|editor)$" AND IS_DIRECTORY "${data}/${entry}")
        # Maps the retail tree keeps loose, next to the archives.
        list(APPEND loose_map_roots "${data}/${entry}")
    endif()
endforeach()
foreach(required spr-sprites_0.bos tiles_0.bos entities_0.bos mis-main_0.bos)
    if(NOT required IN_LIST archives_lower)
        fot_skip("FOT_DATA_DIR=${data} does not look like Fallout Tactics' core folder: there is no ${required} in it")
    endif()
endforeach()

if(DEFINED SHARD)
    set(here "${WORK}/${MODE}-${SHARD}")
else()
    set(here "${WORK}/${MODE}")
endif()
file(MAKE_DIRECTORY "${here}")

# Run a program with its working directory under WORK; stdout and stderr
# together land in <prefix>_out and the exit status in <prefix>_rc.
function(fot_run prefix)
    execute_process(COMMAND ${ARGN}
        WORKING_DIRECTORY "${here}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out
    )
    set(${prefix}_rc "${rc}" PARENT_SCOPE)
    set(${prefix}_out "${out}" PARENT_SCOPE)
endfunction()

# How many files under <dir> have a name ending in <ext> (any case). This
# counts on the raw string rather than walking it as a list: CMake's list
# splitting does not split inside [ ], and two shipped tile names carry an
# unbalanced bracket (Village_Wall_Wood_Fence_O1_1]_SE.til), which would
# glue hundreds of entries together.
function(fot_count_files out_var dir ext)
    file(GLOB_RECURSE found LIST_DIRECTORIES false "${dir}/*")
    string(TOLOWER "${found}" raw)
    string(REGEX REPLACE "\\${ext}(;|$)" "\n" raw "${raw}")
    string(REGEX MATCHALL "\n" hits "${raw}")
    list(LENGTH hits count)
    set(${out_var} ${count} PARENT_SCOPE)
endfunction()

# Every file under <dir> whose name ends in <ext> (any case), sorted. Fails
# loudly, rather than testing fewer files, if a name defeats CMake's lists.
function(fot_find_files out_var dir ext)
    file(GLOB_RECURSE found LIST_DIRECTORIES false "${dir}/*")
    set(result "")
    foreach(f IN LISTS found)
        string(TOLOWER "${f}" lower)
        if(lower MATCHES "\\${ext}$")
            list(APPEND result "${f}")
        endif()
    endforeach()
    list(SORT result)
    list(LENGTH result got)
    fot_count_files(expected "${dir}" "${ext}")
    if(NOT got EQUAL expected)
        message(FATAL_ERROR "${dir} holds ${expected} *${ext} files but only ${got} survive "
            "CMake's list handling: a file name there has an unbalanced [ or ] or a ;")
    endif()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(fot_require_unpacked)
    foreach(sub sprites tiles entities campaigns)
        if(NOT IS_DIRECTORY "${unpacked}/${sub}")
            message(FATAL_ERROR "${unpacked}/${sub} is missing: data.unpack has to run first "
                "(ctest runs it automatically, as a fixture of this test)")
        endif()
    endforeach()
endfunction()

# Every map: the unpacked archives' and the loose ones.
function(fot_all_maps out_var)
    fot_find_files(maps "${unpacked}/campaigns" ".mis")
    foreach(root IN LISTS loose_map_roots)
        fot_find_files(more "${root}" ".mis")
        list(APPEND maps ${more})
    endforeach()
    set(${out_var} "${maps}" PARENT_SCOPE)
endfunction()

set(failures "")

# --- unpack (the fixture's first step) ------------------------------------------
# sprview and mapview read loose files, not .bos archives, so unpack the
# archives they need: every sprite, the tiles, the entities and every map.
if(MODE STREQUAL "unpack")
    file(REMOVE_RECURSE "${unpacked}")
    file(MAKE_DIRECTORY "${unpacked}")
    # Ascending name order is the engine's own mount order (common/vfs.h):
    # a per-CD archive patches the base one.
    list(SORT archives)
    set(count 0)
    foreach(archive IN LISTS archives)
        string(TOLOWER "${archive}" lower)
        if(lower MATCHES "^(spr-|tiles_|entities_|mis-)")
            file(ARCHIVE_EXTRACT INPUT "${data}/${archive}" DESTINATION "${unpacked}")
            math(EXPR count "${count} + 1")
        endif()
    endforeach()
    fot_count_files(n_sprites "${unpacked}/sprites" ".spr")
    fot_count_files(n_tiles "${unpacked}/tiles" ".til")
    fot_count_files(n_maps "${unpacked}/campaigns" ".mis")
    message("unpacked ${count} archives from ${data}: ${n_sprites} sprites, ${n_tiles} tiles, ${n_maps} maps")
    return()
endif()

# --- sprview --selftest over every sprite ------------------------------------------
if(MODE STREQUAL "sprview-selftest")
    fot_require_unpacked()
    fot_find_files(sprites "${unpacked}/sprites" ".spr")
    list(LENGTH sprites n)
    if(n EQUAL 0)
        message(FATAL_ERROR "no .spr files under ${unpacked}/sprites")
    endif()
    set(t_files 0)
    set(t_images 0)
    set(t_decoded 0)
    set(t_errors 0)
    set(t_mismatch 0)
    # Batches keep the command line short enough for Windows.
    set(i 0)
    while(i LESS n)
        list(SUBLIST sprites ${i} 100 batch)
        fot_run(r "${SPRVIEW}" --selftest ${batch})
        if(r_out MATCHES "TOTAL sprites=([0-9]+) images=([0-9]+) decoded=([0-9]+) errors=([0-9]+) rect-mismatches=([0-9]+)")
            math(EXPR t_files "${t_files} + ${CMAKE_MATCH_1}")
            math(EXPR t_images "${t_images} + ${CMAKE_MATCH_2}")
            math(EXPR t_decoded "${t_decoded} + ${CMAKE_MATCH_3}")
            math(EXPR t_errors "${t_errors} + ${CMAKE_MATCH_4}")
            math(EXPR t_mismatch "${t_mismatch} + ${CMAKE_MATCH_5}")
        else()
            list(APPEND failures "batch at ${i}: no TOTAL line (exit ${r_rc})")
        endif()
        if(NOT r_rc STREQUAL "0")
            string(REGEX MATCHALL "[^\n]*(FAIL|err=[1-9]|rectbad=[1-9]|error)[^\n]*" bad "${r_out}")
            list(APPEND failures "batch at ${i} exited ${r_rc}: ${bad}")
        endif()
        math(EXPR i "${i} + 100")
    endwhile()
    message("sprview --selftest: ${t_files} of ${n} sprites, ${t_images} images, ${t_decoded} decoded, "
        "${t_errors} errors, ${t_mismatch} rect mismatches")
    if(NOT t_files EQUAL n)
        list(APPEND failures "only ${t_files} of ${n} sprites loaded")
    endif()
    if(NOT t_errors EQUAL 0 OR NOT t_mismatch EQUAL 0 OR NOT t_decoded EQUAL t_images)
        list(APPEND failures "${t_errors} errors, ${t_mismatch} rect mismatches, ${t_decoded}/${t_images} decoded")
    endif()

# --- sprview --info / --dump / --composite on a sample ------------------------------
elseif(MODE STREQUAL "sprview-dump")
    fot_require_unpacked()
    fot_find_files(sample "${unpacked}/sprites/misc" ".spr")
    fot_find_files(critters "${unpacked}/sprites/critters" ".spr")
    foreach(c IN LISTS critters)
        if(c MATCHES "/MutantDog\\.spr$")
            list(APPEND sample "${c}")
        endif()
    endforeach()
    list(LENGTH sample n)
    if(n EQUAL 0)
        message(FATAL_ERROR "no sample sprites under ${unpacked}/sprites/misc")
    endif()
    file(MAKE_DIRECTORY "${here}/dump" "${here}/composite")
    set(t_written 0)
    set(t_composited 0)
    foreach(spr IN LISTS sample)
        file(RELATIVE_PATH rel "${unpacked}/sprites" "${spr}")
        string(REGEX REPLACE "[/ ]" "_" name "${rel}")
        fot_run(info "${SPRVIEW}" "${spr}" --info)
        if(NOT info_rc STREQUAL "0" OR NOT info_out MATCHES "tag +<sprite> version")
            list(APPEND failures "${rel} --info exited ${info_rc}")
        endif()
        fot_run(dump "${SPRVIEW}" "${spr}" --dump "${here}/dump/${name}")
        if(dump_out MATCHES "images: ([0-9]+)  decoded: ([0-9]+)  png written: ([0-9]+)  errors: ([0-9]+)")
            if(NOT dump_rc STREQUAL "0" OR NOT CMAKE_MATCH_4 EQUAL 0 OR NOT CMAKE_MATCH_3 EQUAL CMAKE_MATCH_1)
                list(APPEND failures "${rel} --dump: ${CMAKE_MATCH_0} (exit ${dump_rc})")
            endif()
            math(EXPR t_written "${t_written} + ${CMAKE_MATCH_3}")
        else()
            list(APPEND failures "${rel} --dump exited ${dump_rc}: ${dump_out}")
        endif()
        fot_run(comp "${SPRVIEW}" "${spr}" --composite "${here}/composite/${name}")
        if(comp_rc STREQUAL "0" AND comp_out MATCHES "composited: ([0-9]+)")
            math(EXPR t_composited "${t_composited} + ${CMAKE_MATCH_1}")
        else()
            list(APPEND failures "${rel} --composite exited ${comp_rc}: ${comp_out}")
        endif()
    endforeach()
    message("sprview on ${n} sprites: --info ok, ${t_written} PNGs dumped, ${t_composited} frames composited")

# --- sprtrim --selftest over every sprite, plus one real trim ------------------------
elseif(MODE STREQUAL "sprtrim-selftest")
    fot_require_unpacked()
    fot_find_files(sprites "${unpacked}/sprites" ".spr")
    list(LENGTH sprites n)
    if(n EQUAL 0)
        message(FATAL_ERROR "no .spr files under ${unpacked}/sprites")
    endif()
    set(t_files 0)
    set(t_bad 0)
    set(i 0)
    while(i LESS n)
        list(SUBLIST sprites ${i} 100 batch)
        fot_run(r "${SPRTRIM}" --selftest ${batch})
        if(r_out MATCHES "sprtrim selftest: ([0-9]+) files, ([0-9]+) bad")
            math(EXPR t_files "${t_files} + ${CMAKE_MATCH_1}")
            math(EXPR t_bad "${t_bad} + ${CMAKE_MATCH_2}")
        else()
            list(APPEND failures "batch at ${i}: no summary line (exit ${r_rc})")
        endif()
        if(NOT r_rc STREQUAL "0")
            string(REGEX MATCHALL "[^\n]*(failed|does not verify)[^\n]*" bad "${r_out}")
            list(APPEND failures "batch at ${i} exited ${r_rc}: ${bad}")
        endif()
        math(EXPR i "${i} + 100")
    endwhile()
    message("sprtrim --selftest: ${t_files} of ${n} sprites, ${t_bad} bad")
    if(NOT t_files EQUAL n OR NOT t_bad EQUAL 0)
        list(APPEND failures "${t_bad} bad, ${t_files} of ${n} checked")
    endif()

    # A real trim to a keep-set, the way `fotgame --sprite-manifest` feeds it,
    # and the result must load again.
    list(GET sprites 0 first)
    set(input "${first}")
    foreach(s IN LISTS sprites)
        if(s MATCHES "/MutantDog\\.spr$")
            set(input "${s}")
        endif()
    endforeach()
    file(WRITE "${here}/keep.txt" "# sequences to keep\nstand\nwalk\n")
    fot_run(trim "${SPRTRIM}" --keep "${here}/keep.txt" "${input}" "${here}/trimmed.spr")
    fot_run(info "${SPRVIEW}" "${here}/trimmed.spr" --info)
    fot_run(report "${SPRTRIM}" --report "${here}/trimmed.spr")
    if(NOT trim_rc STREQUAL "0" OR NOT info_rc STREQUAL "0" OR NOT report_rc STREQUAL "0")
        list(APPEND failures "trim of ${input}: sprtrim exited ${trim_rc} (${trim_out}), "
            "sprview --info ${info_rc}, sprtrim --report ${report_rc}")
    else()
        string(STRIP "${trim_out}" trim_out)
        message("${trim_out}")
    endif()

# --- mapview --selftest over every map -----------------------------------------------
elseif(MODE STREQUAL "mapview-selftest")
    fot_require_unpacked()
    fot_run(r "${MAPVIEW}" --selftest
        --tiles "${unpacked}/tiles" --sprites "${unpacked}/sprites" --ents "${unpacked}/entities"
        "${unpacked}/campaigns" ${loose_map_roots})
    set(maps 0)
    set(parse_errors -1)
    set(tiles_missing -1)
    set(esh_errors -1)
    if(r_out MATCHES "\nmaps parsed: ([0-9]+)")
        set(maps ${CMAKE_MATCH_1})
    endif()
    if(r_out MATCHES "\nparse errors: ([0-9]+)")
        set(parse_errors ${CMAKE_MATCH_1})
    endif()
    if(r_out MATCHES "\ntile prototypes resolved/missing: ([0-9]+)/([0-9]+)")
        set(tiles_resolved ${CMAKE_MATCH_1})
        set(tiles_missing ${CMAKE_MATCH_2})
    endif()
    if(r_out MATCHES "\ntagged-property parse errors: ([0-9]+)")
        set(esh_errors ${CMAKE_MATCH_1})
    endif()
    set(summary "")
    foreach(key "entities parsed: [^\n]*" "sprites resolved/missing: [^\n]*" ".ent prototypes missing: [^\n]*")
        if(r_out MATCHES "\n(${key})")
            string(APPEND summary "; ${CMAKE_MATCH_1}")
        endif()
    endforeach()
    message("mapview --selftest: ${maps} maps parsed, ${parse_errors} parse errors, "
        "tile prototypes ${tiles_resolved}/${tiles_missing} resolved/missing, "
        "${esh_errors} tagged-property errors${summary}")
    if(NOT r_rc STREQUAL "0")
        string(REGEX MATCHALL "\nFAIL[^\n]*" bad "${r_out}")
        list(APPEND failures "mapview exited ${r_rc}${bad}")
    endif()
    if(maps LESS 85)
        list(APPEND failures "only ${maps} maps parsed; the milestone is 85 or more")
    endif()
    if(NOT parse_errors EQUAL 0 OR NOT tiles_missing EQUAL 0 OR NOT esh_errors EQUAL 0)
        list(APPEND failures "parse errors ${parse_errors}, tile prototypes missing ${tiles_missing}, "
            "tagged-property errors ${esh_errors}")
    endif()

# --- mapview --render, one shard of every map ------------------------------------------
elseif(MODE STREQUAL "mapview-render")
    fot_require_unpacked()
    fot_all_maps(maps)
    list(LENGTH maps n)
    set(index 0)
    set(rendered 0)
    foreach(map IN LISTS maps)
        math(EXPR mine "${index} % ${SHARDS} + 1")
        math(EXPR index "${index} + 1")
        if(NOT mine EQUAL SHARD)
            continue()
        endif()
        # The archives' maps and the loose ones share names, so tell them apart.
        string(FIND "${map}" "${unpacked}/" at)
        if(at EQUAL 0)
            file(RELATIVE_PATH rel "${unpacked}" "${map}")
            set(rel "archive/${rel}")
        else()
            file(RELATIVE_PATH rel "${data}" "${map}")
            set(rel "loose/${rel}")
        endif()
        string(REGEX REPLACE "[/ ]" "_" name "${rel}")
        set(png "${here}/${name}.png")
        fot_run(r "${MAPVIEW}" "${map}"
            --tiles "${unpacked}/tiles" --sprites "${unpacked}/sprites" --ents "${unpacked}/entities"
            --render "${png}" --scale 8 --quiet)
        set(size 0)
        if(EXISTS "${png}")
            file(SIZE "${png}" size)
        endif()
        if(NOT r_rc STREQUAL "0" OR size EQUAL 0)
            string(STRIP "${r_out}" r_out)
            list(APPEND failures "${rel}: exit ${r_rc}, ${size} bytes: ${r_out}")
        else()
            math(EXPR rendered "${rendered} + 1")
        endif()
    endforeach()
    message("mapview --render (shard ${SHARD} of ${SHARDS}): ${rendered} maps rendered, of ${n} in all")
    if(rendered EQUAL 0)
        list(APPEND failures "nothing rendered")
    endif()

# --- fotgame: every weapon .ent through the resource layer ------------------------------
elseif(MODE STREQUAL "fotgame-weapons")
    fot_run(r "${FOTGAME}" --assets "${data}" --weapon-dump)
    string(REGEX MATCHALL "WEAPON ent=" weapons "${r_out}")
    list(LENGTH weapons n)
    if(r_out MATCHES "resources: [^\n]*")
        message("${CMAKE_MATCH_0}")
    endif()
    message("fotgame --weapon-dump: ${n} weapons")
    if(NOT r_rc STREQUAL "0" OR n EQUAL 0)
        list(APPEND failures "fotgame exited ${r_rc} with ${n} weapons: ${r_out}")
    endif()

# --- fotgame: Brahmin Wood, headless, with a six-recruit squad; then its audio -----------
elseif(MODE STREQUAL "fotgame-mission")
    set(squad "entities/recruits/hm01.ent,entities/recruits/hm02.ent,entities/recruits/hm03.ent,entities/recruits/hf01.ent,entities/recruits/hf02.ent,entities/recruits/hf03.ent")
    fot_run(r "${FOTGAME}" campaigns/missions/core/mission01.mis --assets "${data}"
        --campaign-start --squad "${squad}"
        --headless --frames 150 --no-sound
        --save-dir "${here}/saves" --screenshot "${here}/mission01.png")
    if(NOT r_rc STREQUAL "0" OR NOT r_out MATCHES "squad 6 placed" OR NOT EXISTS "${here}/mission01.png")
        list(APPEND failures "mission run exited ${r_rc}: ${r_out}")
    else()
        string(REGEX MATCH "spawn point[^\n]*squad 6 placed" spawn "${r_out}")
        string(REGEX MATCH "render: [^\n]*" render "${r_out}")
        message("fotgame mission01: ${spawn}; ${render}")
    endif()
    fot_run(a "${FOTGAME}" campaigns/missions/core/mission01.mis --assets "${data}"
        --headless --frames 120
        --save-dir "${here}/saves" --audio-dump "${here}/mission01.wav")
    if(NOT a_rc STREQUAL "0" OR NOT a_out MATCHES "audio-dump: [^\n]*" OR NOT EXISTS "${here}/mission01.wav")
        list(APPEND failures "audio run exited ${a_rc}: ${a_out}")
    else()
        message("fotgame ${CMAKE_MATCH_0}")
    endif()

# --- fotgame: the campaign's main menu, headless -------------------------------------------
elseif(MODE STREQUAL "fotgame-campaign")
    fot_run(r "${FOTGAME}" --campaign --assets "${data}"
        --headless --frames 60 --no-sound
        --save-dir "${here}/saves" --screenshot "${here}/menu.png")
    if(NOT r_rc STREQUAL "0" OR NOT r_out MATCHES "campaign: campaign v[0-9]+" OR NOT EXISTS "${here}/menu.png")
        list(APPEND failures "campaign run exited ${r_rc}: ${r_out}")
    else()
        message("fotgame --campaign: ${CMAKE_MATCH_0}")
    endif()

else()
    message(FATAL_ERROR "unknown MODE '${MODE}'")
endif()

if(failures)
    list(JOIN failures "\n  " text)
    message(FATAL_ERROR "data.${MODE}: FAILED\n  ${text}")
endif()
