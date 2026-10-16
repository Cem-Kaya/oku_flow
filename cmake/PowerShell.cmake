include_guard(GLOBAL)

# Windows Store app-execution aliases can launch pwsh but cannot be opened for
# hashing. CTest and its evidence must name the actual interpreter image.
function(okuflow_find_powershell variable)
    # Recover stale versioned paths left by older caches after Store updates.
    if(DEFINED ${variable} AND NOT EXISTS "${${variable}}")
        unset(${variable} CACHE)
        set(${variable} "${variable}-NOTFOUND")
    endif()
    find_program(${variable} NAMES pwsh.exe pwsh REQUIRED)
    execute_process(
        COMMAND "${${variable}}" -NoProfile -NonInteractive -Command
            "[Console]::Write([Diagnostics.Process]::GetCurrentProcess().MainModule.FileName)"
        RESULT_VARIABLE result OUTPUT_VARIABLE image ERROR_VARIABLE diagnostic
        OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
    if(NOT result STREQUAL "0" OR NOT IS_ABSOLUTE "${image}" OR NOT EXISTS "${image}")
        message(FATAL_ERROR "Cannot resolve the PowerShell interpreter image from ${${variable}}: ${diagnostic}")
    endif()
    file(TO_CMAKE_PATH "${image}" image)
    # Cache the launcher (which may be a stable Store alias), but give callers
    # the current readable image for generated commands and evidence hashing.
    set(${variable} "${image}" PARENT_SCOPE)
endfunction()
