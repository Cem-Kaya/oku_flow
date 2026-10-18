# Include after recording_manager_tests is defined. This row alone enables
# actual MF codecs; the default manager fixture keeps its desktop-free policy
# checks and reports the native slot as opt-in.
if(TARGET recording_manager_tests)
    add_test(NAME recording_original_native
        COMMAND recording_manager_tests nativeOriginalRecordsOneVideoAndAudioTrack)
    set_tests_properties(recording_original_native PROPERTIES
        TIMEOUT 60
        LABELS "codec"
        RUN_SERIAL TRUE
        ENVIRONMENT "OKUFLOW_TEST_ORIGINAL_RECORDING_NATIVE=1;QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
endif()
