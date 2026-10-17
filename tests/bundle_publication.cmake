if(WIN32)
    include("${PROJECT_SOURCE_DIR}/cmake/PowerShell.cmake")
    okuflow_find_powershell(OKUFLOW_PWSH_EXECUTABLE)
    add_test(NAME bundle_publication_contract COMMAND "${OKUFLOW_PWSH_EXECUTABLE}" -NoProfile -File
        "${PROJECT_SOURCE_DIR}/scripts/test_bundle_publication.ps1")
    add_test(NAME bundle_toolchain_contract COMMAND "${OKUFLOW_PWSH_EXECUTABLE}" -NoProfile -File
        "${PROJECT_SOURCE_DIR}/scripts/test_bundle_toolchain.ps1")
    get_filename_component(bundle_fixture_qt_prefix "${QT_BIN_DIRECTORY}" DIRECTORY)
    set_property(TEST bundle_toolchain_contract APPEND PROPERTY ENVIRONMENT
        "QT_ROOT_DIR=${bundle_fixture_qt_prefix}")
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
        "$ENV{ComSpec}"
        "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe"
        "${QT_QMAKE_EXECUTABLE}"
        "${QT_BIN_DIRECTORY}/windeployqt.exe")
    set(bundle_fixture_qt_source "$ENV{QT_SOURCE_ROOT}")
    if(NOT bundle_fixture_qt_source)
        get_filename_component(bundle_fixture_qt_source "${bundle_fixture_qt_prefix}/../Src" ABSOLUTE)
    endif()
    set_property(TEST bundle_toolchain_contract APPEND PROPERTY ENVIRONMENT
        "QT_SOURCE_ROOT=${bundle_fixture_qt_source}")
    set(bundle_fixture_qt_license "${bundle_fixture_qt_prefix}/LICENSES/LGPL-3.0-only.txt")
    if(NOT EXISTS "${bundle_fixture_qt_license}")
        get_filename_component(bundle_fixture_qt_license "${bundle_fixture_qt_prefix}/../../Licenses/LICENSE" ABSOLUTE)
    endif()
    set_property(TEST bundle_toolchain_contract APPEND PROPERTY REQUIRED_FILES
        "${bundle_fixture_qt_license}"
        "${bundle_fixture_qt_source}/qtmultimedia/src/3rdparty/ffmpeg/LICENSE.LGPL-2.1-or-later.txt")
    foreach(module IN ITEMS qtbase qtimageformats qtmultimedia qtspeech qtsvg)
        set_property(TEST bundle_toolchain_contract APPEND PROPERTY REQUIRED_FILES
            "${bundle_fixture_qt_prefix}/sbom/${module}-${Qt6_VERSION}.spdx.json")
    endforeach()
endif()
