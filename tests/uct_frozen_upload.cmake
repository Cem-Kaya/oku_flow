# The actual cancellation guard and AsyncCleanup ownership contract, with a
# deterministic native-completion gate. No Qt, CUDA device, TRT/cache or model.
add_executable(uct_frozen_upload_tests uct_frozen_upload_tests.cpp)
target_include_directories(uct_frozen_upload_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(uct_frozen_upload_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF DISABLE_PRECOMPILE_HEADERS ON)
add_test(NAME uct_frozen_upload_contract COMMAND uct_frozen_upload_tests)
set_tests_properties(uct_frozen_upload_contract PROPERTIES TIMEOUT 20 LABELS "headless"
    REQUIRED_FILES "${PROJECT_SOURCE_DIR}/src/common/uct_frozen_upload.hpp;${PROJECT_SOURCE_DIR}/src/common/uct_tensorrt.cpp;${PROJECT_SOURCE_DIR}/include/okuflow/common/async_cleanup.hpp;${PROJECT_SOURCE_DIR}/tests/uct_frozen_upload_tests.cpp")
