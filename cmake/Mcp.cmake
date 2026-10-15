# Development catalog is deliberately excluded from bundle/MSI inventories.
# Separate compilation keeps the console bridge Qt-free and /MT, while the
# endpoint uses the application's normal Qt/CRT configuration.
option(OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY
    "Verify the deployed sibling OkuFlowApp.exe (does not promote catalog or enable packaging)" OFF)
set(OKUFLOW_CONTROL_CATALOG_EDITION "dev-camera" CACHE STRING "Immutable agent-control catalog edition")
set_property(CACHE OKUFLOW_CONTROL_CATALOG_EDITION PROPERTY STRINGS dev-core dev-capture dev-camera v1 dev-full)
set(OKUFLOW_CONTROL_CATALOG_EDITIONS dev-core dev-capture dev-camera v1 dev-full)
list(FIND OKUFLOW_CONTROL_CATALOG_EDITIONS "${OKUFLOW_CONTROL_CATALOG_EDITION}" OKUFLOW_CONTROL_CATALOG_EDITION_INDEX)
if(OKUFLOW_CONTROL_CATALOG_EDITION_INDEX LESS 0)
    message(FATAL_ERROR "Unsupported immutable control catalog edition: ${OKUFLOW_CONTROL_CATALOG_EDITION}")
endif()
target_compile_definitions(oku_flow PRIVATE OKUFLOW_CONTROL_CATALOG_EDITION_INDEX=${OKUFLOW_CONTROL_CATALOG_EDITION_INDEX})
set(OKUFLOW_MCP_COMMON
    ${PROJECT_SOURCE_DIR}/src/app/agent/parameter_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/json.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/protocol.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/windows_pipe.cpp)
add_executable(okuflow_mcp
    ${OKUFLOW_MCP_COMMON}
    ${PROJECT_SOURCE_DIR}/src/mcp/pipe_channel.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/main.cpp
    ${PROJECT_SOURCE_DIR}/src/app/agent/control_registry.cpp)
target_include_directories(okuflow_mcp PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(okuflow_mcp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_definitions(okuflow_mcp PRIVATE OKUFLOW_CONTROL_CATALOG_EDITION_INDEX=${OKUFLOW_CONTROL_CATALOG_EDITION_INDEX})
if(NOT OKUFLOW_MCP_DEPLOYED_IMAGE_POLICY)
    target_compile_definitions(okuflow_mcp PRIVATE OKUFLOW_MCP_DEVELOPMENT_IMAGE=1)
endif()
target_compile_options(okuflow_mcp PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(okuflow_mcp PRIVATE advapi32 bcrypt shell32 ole32)
set_target_properties(okuflow_mcp PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_sources(oku_flow PRIVATE
    ${PROJECT_SOURCE_DIR}/src/mcp/app_ipc_endpoint.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/json.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/protocol.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/windows_pipe.cpp)
target_link_libraries(oku_flow PRIVATE advapi32 bcrypt shell32 ole32)
