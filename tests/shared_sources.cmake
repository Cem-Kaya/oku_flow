# Keep compile contracts intact: even an otherwise unrelated fixture define is
# a separate variant. Resources and QObject headers stay on the executables, so
# RCC initialization and each fixture's AUTOMOC inventory are unchanged.
set(okuflow_test_compile_properties
    INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES
    MSVC_RUNTIME_LIBRARY CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS
    POSITION_INDEPENDENT_CODE QT_SKIP_DEFAULT_TESTCASE_DIRS QT_TESTCASE_BUILDDIR
    CUDA_STANDARD CUDA_STANDARD_REQUIRED CUDA_ARCHITECTURES
    CUDA_SEPARABLE_COMPILATION CUDA_RUNTIME_LIBRARY)

function(okuflow_test_compile_contract target output)
    get_target_property(libraries ${target} LINK_LIBRARIES)
    set(usage_libraries "")
    foreach(library IN LISTS libraries)
        if(TARGET ${library})
            list(APPEND usage_libraries "${library}")
        endif()
    endforeach()
    list(SORT usage_libraries)
    set(contract "${usage_libraries}")
    foreach(property IN LISTS okuflow_test_compile_properties)
        get_target_property(value ${target} ${property})
        if(value MATCHES "-NOTFOUND$")
            set(value "")
        endif()
        if(property STREQUAL "COMPILE_DEFINITIONS")
            # Qt's Windows platform interface already supplies these two exact
            # definitions. Explicit repeats do not create a different variant.
            if(WIN32 AND "Qt6::Core" IN_LIST usage_libraries)
                list(REMOVE_ITEM value UNICODE _UNICODE)
            endif()
            list(SORT value)
        endif()
        string(APPEND contract "\n${property}=${value}")
    endforeach()
    set(${output} "${contract}" PARENT_SCOPE)
endfunction()

function(okuflow_share_test_sources name)
    cmake_parse_arguments(PARSE_ARGV 1 shared "" "" "TARGETS;SOURCES")
    list(GET shared_TARGETS 0 reference)
    okuflow_test_compile_contract(${reference} reference_contract)
    add_library(${name} OBJECT ${shared_SOURCES})
    set_target_properties(${name} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    foreach(property IN LISTS okuflow_test_compile_properties)
        get_target_property(value ${reference} ${property})
        if(NOT value STREQUAL "" AND NOT value MATCHES "-NOTFOUND$")
            set_property(TARGET ${name} PROPERTY ${property} "${value}")
        endif()
    endforeach()
    # Native import libraries have no compile usage requirements. Retain every
    # Qt dependency, including Qt Test's default testcase-directory definitions.
    get_target_property(libraries ${reference} LINK_LIBRARIES)
    foreach(library IN LISTS libraries)
        if(TARGET ${library})
            target_link_libraries(${name} PRIVATE ${library})
        endif()
    endforeach()
    foreach(target IN LISTS shared_TARGETS)
        okuflow_test_compile_contract(${target} contract)
        if(NOT contract STREQUAL reference_contract)
            message(FATAL_ERROR "${name}: ${target} differs from ${reference}; create a separate compile variant")
        endif()
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS shared_SOURCES)
            if(NOT source IN_LIST sources)
                message(FATAL_ERROR "${target} does not compile shared source ${source}")
            endif()
            list(REMOVE_ITEM sources "${source}")
        endforeach()
        set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:${name}>)
    endforeach()
endfunction()

okuflow_share_test_sources(okuflow_test_widget_translation
    TARGETS color_scheme_picker_tests annotation_key_routing_tests recording_manager_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp)
okuflow_share_test_sources(okuflow_test_widget_geometry
    TARGETS annotation_key_routing_tests recording_manager_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp
        ${PROJECT_SOURCE_DIR}/src/common/annotation_model.cpp
        ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)
okuflow_share_test_sources(okuflow_test_language_status
    TARGETS language_manager_tests live_status_text_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/live_status_text.cpp)
okuflow_share_test_sources(okuflow_test_view_transform
    TARGETS view_transform_tests annotation_model_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)
okuflow_share_test_sources(okuflow_test_cpu_image
    TARGETS yuv_color_tests recording_integrity_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/frame_pipeline.cpp
        ${PROJECT_SOURCE_DIR}/src/common/image_processing.cpp)
okuflow_share_test_sources(okuflow_test_media_writer
    TARGETS media_writer_audio_tests media_writer_gpu_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/media_writer.cpp
        ${PROJECT_SOURCE_DIR}/src/common/recording_contract.cpp)
okuflow_share_test_sources(okuflow_test_visible_chat
    TARGETS visible_chat_session_tests visible_chat_session_store_tests
    SOURCES ${PROJECT_SOURCE_DIR}/src/common/visible_chat_session.cpp)

if(OKUFLOW_ENABLE_CUDA)
    # These four fixtures have identical nvcc flags and no per-fixture defines.
    # OBJECT inclusion preserves the original non-separable CUDA link behavior.
    okuflow_share_test_sources(okuflow_test_cuda_kernels
        TARGETS yuv_color_cuda_tests text_clarity_cuda_tests stabilization_cuda_tests
            stabilization_replay_cuda
        SOURCES ${PROJECT_SOURCE_DIR}/src/cuda/cuda_kernels.cu)
    set_target_properties(okuflow_test_cuda_kernels PROPERTIES
        CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES "${OKUFLOW_CUDA_ARCHITECTURES}")
endif()
