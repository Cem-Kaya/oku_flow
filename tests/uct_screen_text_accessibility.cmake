# Ultra Clear Text on-screen text exposed through QAccessible (UI Automation
# source model). Desktop-free Qt offscreen contract tests; they do not prove
# native UIA/Narrator/NVDA/Magnifier behaviour. Each scale runs separately so
# geometry is checked at 100%, 150% and 200% logical-to-device scaling.
if(NOT WIN32)
    return()
endif()

add_executable(uct_screen_text_accessibility_tests
    uct_screen_text_accessibility_tests.cpp
    ultra_clear_text_test_support.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/common/ultra_clear_text.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/uct_screen_text_accessible.hpp
    ${PROJECT_SOURCE_DIR}/include/okuflow/ui/ultra_clear_text_overlay.hpp
    ${PROJECT_SOURCE_DIR}/src/common/ultra_clear_text.cpp
    ${PROJECT_SOURCE_DIR}/src/common/view_transform.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/uct_screen_text_accessible.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ultra_clear_text_overlay.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/translation_catalog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ui_translation.cpp)
target_include_directories(uct_screen_text_accessibility_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(uct_screen_text_accessibility_tests PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(uct_screen_text_accessibility_tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)

foreach(scale IN ITEMS 1 1.5 2)
    string(REPLACE "." "_" scale_name "${scale}")
    add_test(NAME uct_screen_text_accessibility_scale_${scale_name}
        COMMAND uct_screen_text_accessibility_tests)
    set_tests_properties(uct_screen_text_accessibility_scale_${scale_name} PROPERTIES
        TIMEOUT 120 LABELS "headless"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale};QT_FORCE_STDERR_LOGGING=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIRECTORY}")
endforeach()
