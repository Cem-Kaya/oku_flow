if(WIN32)
    include("${PROJECT_SOURCE_DIR}/cmake/PowerShell.cmake")
    okuflow_find_powershell(OKUFLOW_PWSH_EXECUTABLE)
    set(uct_distribution_inputs
        "${PROJECT_SOURCE_DIR}/scripts/stage_uct_payload.ps1"
        "${PROJECT_SOURCE_DIR}/scripts/uct/distribution.json"
        "${PROJECT_SOURCE_DIR}/scripts/uct/pins.json"
        "${PROJECT_SOURCE_DIR}/scripts/uct/bf16-models.json"
        "${PROJECT_SOURCE_DIR}/scripts/uct/precision_models.py"
        "${PROJECT_SOURCE_DIR}/include/okuflow/common/uct_model_pins.hpp"
        "${PROJECT_SOURCE_DIR}/src/common/uct_runtime_precision.hpp"
        "${PROJECT_SOURCE_DIR}/src/common/uct_public_bf16.hpp"
        "${PROJECT_SOURCE_DIR}/src/common/uct_bf16_detector.hpp"
        "${OKUFLOW_PWSH_EXECUTABLE}")
    foreach(case IN ITEMS payload bundle_metadata)
        add_test(NAME "uct_${case}_contract"
            COMMAND "${OKUFLOW_PWSH_EXECUTABLE}" -NoProfile -File
                "${PROJECT_SOURCE_DIR}/scripts/test_uct_${case}.ps1")
        set_tests_properties("uct_${case}_contract" PROPERTIES
            TIMEOUT 120 LABELS "headless;packaging" RUN_SERIAL TRUE
            REQUIRED_FILES "${uct_distribution_inputs};${PROJECT_SOURCE_DIR}/scripts/test_uct_${case}.ps1;${PROJECT_SOURCE_DIR}/scripts/windows-installer/uct_bundle.ps1")
    endforeach()
endif()
