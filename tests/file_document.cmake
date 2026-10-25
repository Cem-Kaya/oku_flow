add_executable(file_document_tests file_document_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/common/file_document.cpp)
target_include_directories(file_document_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(file_document_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Pdf Qt6::Test)
target_compile_definitions(file_document_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(file_document_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME file_document_contract COMMAND file_document_tests)
set_tests_properties(file_document_contract PROPERTIES TIMEOUT 30 LABELS "headless"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}"
    REQUIRED_FILES "${PROJECT_SOURCE_DIR}/src/common/file_document.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/file_document.hpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/media_frame_layout.hpp;$<TARGET_FILE:Qt6::Pdf>")
