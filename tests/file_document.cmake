add_executable(file_document_tests file_document_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/common/file_document.cpp
    ${PROJECT_SOURCE_DIR}/src/common/file_annotations.cpp
    ${PROJECT_SOURCE_DIR}/src/common/annotation_model.cpp
    ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp)
target_include_directories(file_document_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(file_document_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Pdf Qt6::Test)
target_compile_definitions(file_document_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(file_document_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
if(DEFINED ENV{OKUFLOW_QPDF_ROOT})
    set(_qpdf_runtime "$ENV{OKUFLOW_QPDF_ROOT}")
else()
    set(_qpdf_runtime "$ENV{LOCALAPPDATA}/OkuFlow/qpdf/12.4.2")
endif()
file(TO_CMAKE_PATH "${_qpdf_runtime}" _qpdf_runtime)
if(TARGET native_app_control_fixture)
    set_property(TEST native_app_control_offscreen native_app_file_sources_offscreen APPEND PROPERTY
        REQUIRED_FILES "${_qpdf_runtime}/bin/qpdf.exe;${_qpdf_runtime}/bin/qpdf30.dll;${PROJECT_SOURCE_DIR}/include/okuflow/common/file_annotations.hpp;${PROJECT_SOURCE_DIR}/src/common/file_annotations.cpp")
endif()
add_test(NAME file_document_contract COMMAND file_document_tests)
set_tests_properties(file_document_contract PROPERTIES TIMEOUT 30 LABELS "headless"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1;OKUFLOW_QPDF_ROOT=${_qpdf_runtime}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    REQUIRED_FILES "${PROJECT_SOURCE_DIR}/src/common/file_document.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/file_document.hpp;${PROJECT_SOURCE_DIR}/src/common/file_annotations.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/file_annotations.hpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/media_frame_layout.hpp;${_qpdf_runtime}/bin/qpdf.exe;${_qpdf_runtime}/bin/qpdf30.dll;$<TARGET_FILE:Qt6::Pdf>")
add_test(NAME pdf_export_distribution_contract COMMAND pwsh -NoProfile -File ${PROJECT_SOURCE_DIR}/scripts/test_pdf_export.ps1)
set(_pdf_inputs "${PROJECT_SOURCE_DIR}/scripts/test_pdf_export.ps1;${PROJECT_SOURCE_DIR}/scripts/stage_pdf_export.ps1;${PROJECT_SOURCE_DIR}/scripts/windows-installer/pdf_bundle.ps1;${PROJECT_SOURCE_DIR}/scripts/qpdf/pins.json;${_qpdf_runtime}/bin/qpdf.exe;${_qpdf_runtime}/bin/qpdf30.dll")
foreach(_notice LICENSE.txt NOTICE.md OPENSSL_LICENSE.txt ZLIB_LICENSE.txt LIBJPEG_TURBO_LICENSE.txt)
    list(APPEND _pdf_inputs "${PROJECT_SOURCE_DIR}/scripts/qpdf/${_notice}" "${_qpdf_runtime}/licenses/${_notice}")
endforeach()
set_tests_properties(pdf_export_distribution_contract PROPERTIES TIMEOUT 30 LABELS "headless"
    ENVIRONMENT "OKUFLOW_QPDF_ROOT=${_qpdf_runtime}" REQUIRED_FILES "${_pdf_inputs}")
