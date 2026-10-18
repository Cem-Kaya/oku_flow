# Runtime file inputs belong in the CTest inventory even when compiled into
# an RCC payload. Keep output files and fixture-generated temporary data out.
set_property(TEST cuda_target_policy APPEND PROPERTY REQUIRED_FILES
    "${PROJECT_SOURCE_DIR}/cmake/CudaTargets.cmake")
set_property(TEST agent_control_coverage_contract mcp_bridge_contract APPEND PROPERTY
    REQUIRED_FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/app_control_inventory.json")
set_property(TEST agent_control_contract APPEND PROPERTY
    REQUIRED_FILES "${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp")
set_property(TEST notes_html codex_agent_profile_contract codex_agent_client_contract ${assistive_preparation_cases}
    APPEND PROPERTY REQUIRED_FILES "${PROJECT_SOURCE_DIR}/assets/codex-agent/0.160.1-profile.json")
set_property(TEST phone_camera_contract APPEND PROPERTY REQUIRED_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/data/phone-camera.h264"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/phone-camera.hevc")
set_property(TEST phone_camera_contract APPEND PROPERTY REQUIRED_FILES $<TARGET_FILE:adb_owned_child_fixture>)
set_property(TEST adb_legacy_recovery APPEND PROPERTY REQUIRED_FILES $<TARGET_FILE:adb_legacy_fixture>)
set_property(TEST windows_runtime_contract APPEND PROPERTY REQUIRED_FILES
    $<TARGET_FILE:okuflow_launcher> $<TARGET_FILE:launcher_fixture>)
set_property(TEST codex_agent_client_contract APPEND PROPERTY REQUIRED_FILES $<TARGET_FILE:fake_codex_agent_server>)
set_property(TEST notes_html ${assistive_preparation_cases} APPEND PROPERTY REQUIRED_FILES $<TARGET_FILE:fake_codex_app_server>)
set_property(TEST ${assistive_preparation_cases} APPEND PROPERTY REQUIRED_FILES $<TARGET_FILE:fake_scoped_codex_vision>)
set_property(TEST mcp_bridge_contract APPEND PROPERTY REQUIRED_FILES
    $<TARGET_FILE:mcp_app_fixture> $<TARGET_FILE_DIR:mcp_app_fixture>/okuflow_mcp.exe)
get_property(tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS tests)
    if(test MATCHES "^(setup_assistant_interactions|language_catalog_contract|multilingual_live_switch|color_scheme_picker|main_window_interactions\\.)")
        set_property(TEST "${test}" APPEND PROPERTY REQUIRED_FILES ${OKUFLOW_QM_FILES})
    endif()
endforeach()
if(TEST native_app_control_offscreen)
    set_property(TEST native_app_control_offscreen APPEND PROPERTY REQUIRED_FILES ${OKUFLOW_QM_FILES})
endif()
if(OKUFLOW_ENABLE_CUDA)
    foreach(test IN ITEMS yuv_color_conversion_cuda text_clarity_sauvola_cuda
            stabilization_similarity_cuda spatial_upscaler_reference_cuda stabilization_replay_regression)
        set_property(TEST ${test} APPEND PROPERTY LABELS cuda-variant)
    endforeach()
    if(TEST cuda_architecture_coverage)
        set_property(TEST cuda_architecture_coverage APPEND PROPERTY LABELS cuda-variant)
        set_property(TEST cuda_architecture_coverage APPEND PROPERTY REQUIRED_FILES
            "${OKUFLOW_CUOBJDUMP}" $<TARGET_FILE:oku_flow>)
    endif()
endif()

set_property(TEST uct_paddle_processing APPEND PROPERTY REQUIRED_FILES
    "${PROJECT_SOURCE_DIR}/scripts/uct/pins.json"
    "${PROJECT_SOURCE_DIR}/include/okuflow/common/uct_model_pins.hpp")
