# Register Qt slots independently without recompiling/copying the executable.
# Source definitions are the inventory: new slots enter the complete gate on
# reconfigure automatically; _data helpers remain attached to their test slot.
function(okuflow_add_gui_cases prefix target fixture)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${target}.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(READ "${source}" contents)
    string(REGEX MATCHALL "void ${fixture}::[A-Za-z0-9_]+\\(\\)" definitions "${contents}")
    if(NOT definitions)
        message(FATAL_ERROR "No Qt test slots found in ${source}")
    endif()
    foreach(definition IN LISTS definitions)
        string(REGEX REPLACE "void ${fixture}::([A-Za-z0-9_]+)\\(\\)" "\\1" slot "${definition}")
        if(slot MATCHES "_data$|^(init|cleanup|initTestCase|cleanupTestCase)$")
            continue()
        endif()
        set(group "gui-state")
        if(slot MATCHES "[Ii]dle|[Nn]ativeInput|[Nn]ativeHandoff")
            set(group "gui-idle")
        elseif(slot MATCHES "[Kk]eyboard|[Ff]ocus|[Tt]ravers|[Tt]abAnd|[Tt]extPlacement|[Dd]rag|[Dd]ock|[Ee]dge|[Pp]ull|[Mm]arquee")
            set(group "gui-input")
        elseif(slot MATCHES "[Gg]eometry|[Ll]anguage|[Tt]ranslat|[Cc]hrome|[Nn]arrow|[Rr]esize|[Aa]ppearance|[Rr]eachable")
            set(group "gui-layout")
        endif()
        add_test(NAME "${prefix}.${slot}" COMMAND ${target} "${slot}")
        set_tests_properties("${prefix}.${slot}" PROPERTIES
            TIMEOUT 30 LABELS "gui;${group}"
            RUN_SERIAL TRUE RESOURCE_LOCK okuflow_desktop
            ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
    endforeach()
endfunction()
