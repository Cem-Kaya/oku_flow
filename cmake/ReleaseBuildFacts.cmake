# One configured-target authority for packaging, independent of the shell's
# CUDA_PATH and of requested options that a cache/extra argument can override.
function(okuflow_generate_release_build_facts target)
    set(cuda_enabled false)
    set(cuda_facts "\"compiler\": null, \"compilerVersion\": null, \"compilerSha256\": null, \"toolkitRoot\": null, \"toolkitVersion\": null, \"architectures\": [], \"runtimeLibrary\": null, \"runtimePath\": null, \"runtimeSha256\": null, \"license\": null, \"licenseSha256\": null")
    if(OKUFLOW_ENABLE_CUDA)
        set(cuda_enabled true)
        get_target_property(architectures ${target} CUDA_ARCHITECTURES)
        get_target_property(runtime ${target} CUDA_RUNTIME_LIBRARY)
        get_target_property(libraries ${target} LINK_LIBRARIES)
        if(NOT architectures OR NOT runtime STREQUAL "Static" OR
           NOT CUDAToolkit_VERSION OR NOT CUDAToolkit_NVCC_EXECUTABLE OR
           NOT "CUDA::cudart_static" IN_LIST libraries)
            message(FATAL_ERROR "Release build facts require the effective CUDA target and toolkit")
        endif()
        get_target_property(runtime_path CUDA::cudart_static IMPORTED_LOCATION)
        file(REAL_PATH "${runtime_path}" runtime_path)
        file(SHA256 "${runtime_path}" runtime_sha256)
        file(REAL_PATH "${CMAKE_CUDA_COMPILER}" compiler)
        file(REAL_PATH "${CUDAToolkit_NVCC_EXECUTABLE}" toolkit_compiler)
        if(NOT compiler STREQUAL toolkit_compiler)
            message(FATAL_ERROR "CUDA compiler and linked toolkit differ; use a fresh build directory")
        endif()
        get_filename_component(bin_dir "${compiler}" DIRECTORY)
        get_filename_component(toolkit_root "${bin_dir}" DIRECTORY)
        set(license "")
        foreach(name EULA.txt LICENSE)
            if(EXISTS "${toolkit_root}/${name}")
                set(license "${toolkit_root}/${name}")
                break()
            endif()
        endforeach()
        if(license)
            file(SHA256 "${license}" license_sha256)
        else()
            # Development builds may proceed; packaging refuses absent notices.
            set(license_sha256 "")
        endif()
        file(SHA256 "${compiler}" compiler_sha256)
        string(REPLACE ";" "\", \"" architecture_json "${architectures}")
        set(cuda_facts "\"compiler\": \"${compiler}\", \"compilerVersion\": \"${CMAKE_CUDA_COMPILER_VERSION}\", \"compilerSha256\": \"${compiler_sha256}\", \"toolkitRoot\": \"${toolkit_root}\", \"toolkitVersion\": \"${CUDAToolkit_VERSION}\", \"architectures\": [\"${architecture_json}\"], \"runtimeLibrary\": \"${runtime}\", \"runtimePath\": \"${runtime_path}\", \"runtimeSha256\": \"${runtime_sha256}\", \"license\": \"${license}\", \"licenseSha256\": \"${license_sha256}\"")
    endif()
    # Windows paths are normalized by CMake. Escape the remaining JSON string
    # metacharacters in user-selected source/build locations.
    foreach(name CMAKE_SOURCE_DIR CMAKE_BINARY_DIR CMAKE_GENERATOR)
        string(REPLACE "\\" "/" ${name}_json "${${name}}")
        string(REPLACE "\"" "\\\"" ${name}_json "${${name}_json}")
    endforeach()
    set(facts_path "${CMAKE_BINARY_DIR}/okuflow-build-facts/$<CONFIG>.json")
    file(GENERATE OUTPUT "${facts_path}"
        CONTENT "{\n  \"schema\": 1,\n  \"target\": \"${target}\",\n  \"sourceDirectory\": \"${CMAKE_SOURCE_DIR_json}\",\n  \"buildDirectory\": \"${CMAKE_BINARY_DIR_json}\",\n  \"generator\": \"${CMAKE_GENERATOR_json}\",\n  \"configuration\": \"$<CONFIG>\",\n  \"application\": \"$<TARGET_FILE:${target}>\",\n  \"cuda\": {\"enabled\": ${cuda_enabled}, ${cuda_facts}}\n}\n")
    get_target_property(imported ${target} IMPORTED)
    if(NOT imported)
        set(seal_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SealReleaseBuildFacts.cmake")
        # A facts change requires a successful link/seal even when it changes
        # provenance only (for example the toolkit notice), not compiler flags.
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${facts_path}" "${seal_script}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" "-DFactsPath=${facts_path}" -P "${seal_script}"
            VERBATIM COMMENT "Sealing release target bytes and configured build facts")
    endif()
endfunction()
