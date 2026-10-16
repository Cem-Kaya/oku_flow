if(NOT OKUFLOW_USE_PCH)
    return()
endif()

# Shared objects are checked as standalone translation units and use no PCH.
# Amortize headers only on executable fixtures with several remaining sources.
foreach(target IN ITEMS main_window_tests annotation_overlay_tests assistive_preparation_tests
        notes_html_tests agent_control_panel_tests agent_control_coverage_tests
        codex_agent_client_tests setup_assistant_tests recording_manager_tests
        language_manager_tests color_scheme_picker_tests annotation_key_routing_tests
        recording_integrity_tests media_writer_audio_tests media_writer_gpu_tests)
    okuflow_enable_pch(${target})
endforeach()
