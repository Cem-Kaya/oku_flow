if(WIN32)
    include("${PROJECT_SOURCE_DIR}/cmake/PowerShell.cmake")
    okuflow_find_powershell(OKUFLOW_PWSH_EXECUTABLE)
    add_test(NAME owned_build_cleanup_contract
        COMMAND "${OKUFLOW_PWSH_EXECUTABLE}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/scripts/test_owned_build_cleanup.ps1")
    set_tests_properties(owned_build_cleanup_contract PROPERTIES
        TIMEOUT 120 LABELS "headless;packaging" RUN_SERIAL TRUE
        REQUIRED_FILES "${PROJECT_SOURCE_DIR}/scripts/test_owned_build_cleanup.ps1;${PROJECT_SOURCE_DIR}/scripts/windows-build/owned_job_cleanup.ps1;${OKUFLOW_PWSH_EXECUTABLE}")
endif()
