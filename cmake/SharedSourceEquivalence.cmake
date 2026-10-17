# Record consumer ownership before removing production sources. The generated
# compile database supplies the fully resolved compiler environment, including
# transitive Qt usage requirements, without synthetic compilation targets.
include("${PROJECT_SOURCE_DIR}/cmake/PowerShell.cmake")
okuflow_find_powershell(OKUFLOW_EQUIVALENCE_PWSH)
set(OKUFLOW_EQUIVALENCE_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/shared-source-consumers.tsv")
set(OKUFLOW_EQUIVALENCE_ENVIRONMENTS "${CMAKE_CURRENT_BINARY_DIR}/shared-source-environments.tsv")
file(WRITE "${OKUFLOW_EQUIVALENCE_MANIFEST}" "")

function(okuflow_check_shared_source_overrides source target)
    foreach(property COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FLAGS
            INCLUDE_DIRECTORIES SKIP_PRECOMPILE_HEADERS
            COMPILE_DEFINITIONS_DEBUG COMPILE_DEFINITIONS_RELEASE
            COMPILE_DEFINITIONS_RELWITHDEBINFO COMPILE_DEFINITIONS_MINSIZEREL)
        get_source_file_property(value "${source}" TARGET_DIRECTORY ${target} ${property})
        if(value AND NOT value STREQUAL "NOTFOUND")
            message(FATAL_ERROR "Shared source environment: source-specific ${property} on ${target}/${source}")
        endif()
    endforeach()
endfunction()

function(okuflow_record_shared_source_environments)
    file(STRINGS "${OKUFLOW_EQUIVALENCE_MANIFEST}" rows)
    set(targets "")
    set(source_inputs "")
    foreach(row IN LISTS rows)
        string(REPLACE "\t" ";" fields "${row}")
        list(GET fields 1 source)
        list(GET fields 2 target)
        list(APPEND source_inputs "${source}")
        list(APPEND targets "${target}")
    endforeach()
    list(REMOVE_DUPLICATES targets)
    file(WRITE "${OKUFLOW_EQUIVALENCE_ENVIRONMENTS}" "")
    foreach(target IN LISTS targets)
        get_target_property(sources ${target} SOURCES)
        get_target_property(directory ${target} SOURCE_DIR)
        set(witness "")
        foreach(source IN LISTS sources)
            if(source MATCHES "\\.cpp$" AND NOT source MATCHES "\\$<")
                cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${directory}" NORMALIZE OUTPUT_VARIABLE witness)
                okuflow_check_shared_source_overrides("${witness}" ${target})
                break()
            endif()
        endforeach()
        if(NOT witness)
            message(FATAL_ERROR "Shared source environment: no standalone C++ witness on ${target}")
        endif()
        file(APPEND "${OKUFLOW_EQUIVALENCE_ENVIRONMENTS}" "${target}\t${witness}\n")
    endforeach()
    # This script preprocesses source/header bytes at test time. They are not
    # dependencies of its interpreter executable, so expose them to both impact
    # selection and per-test evidence. Headers are conservative: any project
    # header can change conditional preprocessing under a consumer environment.
    file(GLOB_RECURSE header_inputs CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/*.h" "${PROJECT_SOURCE_DIR}/include/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.h" "${PROJECT_SOURCE_DIR}/src/*.hpp"
        "${PROJECT_SOURCE_DIR}/third_party/*.h" "${PROJECT_SOURCE_DIR}/third_party/*.hpp")
    list(REMOVE_DUPLICATES source_inputs)
    set_property(TEST shared_source_equivalence APPEND PROPERTY REQUIRED_FILES
        ${source_inputs} ${header_inputs})
endfunction()
# Resolve witnesses after all source moves and PCH configuration in tests/.
cmake_language(DEFER CALL okuflow_record_shared_source_environments)

foreach(variable INCLUDE CL)
    string(REPLACE "'" "''" EQ_${variable} "$ENV{${variable}}")
endforeach()
string(REPLACE "'" "''" EQ_POST_CL "$ENV{_CL_}")
configure_file("${PROJECT_SOURCE_DIR}/cmake/check_shared_source_equivalence.ps1.in"
    "${CMAKE_CURRENT_BINARY_DIR}/shared_source_equivalence.ps1" @ONLY)
add_test(NAME shared_source_equivalence COMMAND "${OKUFLOW_EQUIVALENCE_PWSH}"
    -NoProfile -File "${CMAKE_CURRENT_BINARY_DIR}/shared_source_equivalence.ps1"
    -BuildDir "${CMAKE_BINARY_DIR}")
set_tests_properties(shared_source_equivalence PROPERTIES LABELS "headless" TIMEOUT 180
    REQUIRED_FILES "${CMAKE_BINARY_DIR}/compile_commands.json;${OKUFLOW_EQUIVALENCE_MANIFEST};${OKUFLOW_EQUIVALENCE_ENVIRONMENTS}")
