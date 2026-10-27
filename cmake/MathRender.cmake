if(NOT TARGET okuflow_math_typesetter)
    set(math_vendor "${PROJECT_SOURCE_DIR}/third_party/microtex")
    file(GLOB_RECURSE math_sources CONFIGURE_DEPENDS
        "${math_vendor}/src/atom/*.cpp" "${math_vendor}/src/box/*.cpp"
        "${math_vendor}/src/core/*.cpp" "${math_vendor}/src/fonts/*.cpp"
        "${math_vendor}/src/utils/*.cpp" "${math_vendor}/src/res/*.cpp")
    add_library(okuflow_math_typesetter STATIC ${math_sources}
        "${math_vendor}/src/latex.cpp" "${math_vendor}/src/render.cpp"
        "${math_vendor}/src/platform/qt/graphic_qt.cpp"
        "${math_vendor}/okuflow_math_fonts.qrc"
        "${PROJECT_SOURCE_DIR}/third_party/tinyxml2/tinyxml2.cpp")
    target_include_directories(okuflow_math_typesetter PUBLIC "${math_vendor}/src"
        PRIVATE "${PROJECT_SOURCE_DIR}/third_party/tinyxml2")
    target_link_libraries(okuflow_math_typesetter PRIVATE Qt6::Core Qt6::Gui)
    target_compile_definitions(okuflow_math_typesetter PUBLIC BUILD_QT PRIVATE NOMINMAX QT_NO_KEYWORDS)
    target_compile_options(okuflow_math_typesetter PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(okuflow_math_typesetter PROPERTIES AUTOMOC OFF AUTOUIC OFF
        DISABLE_PRECOMPILE_HEADERS ON MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
