if(NOT OKUFLOW_USE_PCH)
    return()
endif()

foreach(support IN ITEMS widget_translation language_status view_transform cpu_image media_writer visible_chat)
    okuflow_enable_pch(okuflow_test_${support})
endforeach()
target_precompile_headers(okuflow_test_widget_geometry REUSE_FROM okuflow_test_widget_translation)
foreach(target IN ITEMS color_scheme_picker_tests annotation_key_routing_tests recording_manager_tests)
    target_precompile_headers(${target} REUSE_FROM okuflow_test_widget_translation)
endforeach()
foreach(target IN ITEMS language_manager_tests live_status_text_tests)
    target_precompile_headers(${target} REUSE_FROM okuflow_test_language_status)
endforeach()
foreach(target IN ITEMS view_transform_tests annotation_model_tests)
    target_precompile_headers(${target} REUSE_FROM okuflow_test_view_transform)
endforeach()
foreach(target IN ITEMS yuv_color_tests recording_integrity_tests)
    target_precompile_headers(${target} REUSE_FROM okuflow_test_cpu_image)
endforeach()
foreach(target IN ITEMS media_writer_audio_tests media_writer_gpu_tests)
    target_precompile_headers(${target} REUSE_FROM okuflow_test_media_writer)
endforeach()
foreach(target IN ITEMS visible_chat_session_tests visible_chat_session_store_tests)
    target_precompile_headers(${target} REUSE_FROM okuflow_test_visible_chat)
endforeach()

# Amortize the header build on the larger remaining suites. Small one-file
# contracts deliberately avoid a separate header compilation of their own.
foreach(target IN ITEMS main_window_tests annotation_overlay_tests assistive_preparation_tests
        notes_html_tests agent_control_panel_tests agent_control_coverage_tests
        codex_agent_client_tests setup_assistant_tests)
    okuflow_enable_pch(${target})
endforeach()
# CMake manages each producer's compile PDB (/Fd), including REUSE_FROM.
# Leave the project's debug-information format and CRT selection unchanged.
