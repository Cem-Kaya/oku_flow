add_executable(visible_chat_session_store_tests visible_chat_session_store_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/common/visible_chat_session_store.cpp
    ${PROJECT_SOURCE_DIR}/src/common/visible_chat_session.cpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/visible_chat_session.hpp)
target_include_directories(visible_chat_session_store_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(visible_chat_session_store_tests PRIVATE Qt6::Core Qt6::Test)
target_compile_options(visible_chat_session_store_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME visible_chat_session_store_contract COMMAND visible_chat_session_store_tests)
set_tests_properties(visible_chat_session_store_contract PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
