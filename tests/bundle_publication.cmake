if(WIN32)
    include("${PROJECT_SOURCE_DIR}/cmake/PowerShell.cmake")
    okuflow_find_powershell(OKUFLOW_PWSH_EXECUTABLE)
    add_test(NAME bundle_publication_contract COMMAND "${OKUFLOW_PWSH_EXECUTABLE}" -NoProfile -File
        "${PROJECT_SOURCE_DIR}/scripts/test_bundle_publication.ps1")
    add_test(NAME bundle_toolchain_contract COMMAND "${OKUFLOW_PWSH_EXECUTABLE}" -NoProfile -File
        "${PROJECT_SOURCE_DIR}/scripts/test_bundle_toolchain.ps1")
    set_tests_properties(bundle_publication_contract bundle_toolchain_contract PROPERTIES
        TIMEOUT 60 LABELS "headless;packaging")
    # The publication fixture owns a hidden, normally exiting OkuFlow-named
    # process; serialize against fail-closed native process-identity probes.
    set_tests_properties(bundle_publication_contract PROPERTIES RESOURCE_LOCK okuflow_process_identity)
    set_property(TEST bundle_publication_contract APPEND PROPERTY REQUIRED_FILES
        "${PROJECT_SOURCE_DIR}/scripts/test_bundle_publication.ps1"
        "${PROJECT_SOURCE_DIR}/scripts/publish_release_bundle.ps1"
        "$ENV{ComSpec}")
    set_property(TEST bundle_toolchain_contract APPEND PROPERTY REQUIRED_FILES
        "${PROJECT_SOURCE_DIR}/scripts/test_bundle_toolchain.ps1"
        "${PROJECT_SOURCE_DIR}/scripts/check_bundle_toolchain.ps1"
        "${PROJECT_SOURCE_DIR}/scripts/build_release_bundle.bat"
        "${QT_QMAKE_EXECUTABLE}"
        "${QT_BIN_DIRECTORY}/windeployqt.exe")
endif()
