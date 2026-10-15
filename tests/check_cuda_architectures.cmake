# Asserts that the built application embeds native SASS for every CUDA
# architecture OkuFlow claims to support. A missing architecture only fails
# at kernel-launch time or depend on driver/PTX compatibility, so
# this inventory check runs on the build machine instead. Invoke with:
#   cmake -DOKUFLOW_CUOBJDUMP=<cuobjdump> -DOKUFLOW_EXECUTABLE=<exe>
#         -DOKUFLOW_EXPECTED_ARCHS=75,80,... -P check_cuda_architectures.cmake
if(NOT OKUFLOW_CUOBJDUMP OR NOT OKUFLOW_EXECUTABLE OR NOT OKUFLOW_EXPECTED_ARCHS)
    message(FATAL_ERROR
        "check_cuda_architectures.cmake requires OKUFLOW_CUOBJDUMP, "
        "OKUFLOW_EXECUTABLE and OKUFLOW_EXPECTED_ARCHS")
endif()

execute_process(
    COMMAND "${OKUFLOW_CUOBJDUMP}" --list-elf "${OKUFLOW_EXECUTABLE}"
    OUTPUT_VARIABLE elf_list
    ERROR_VARIABLE elf_error
    RESULT_VARIABLE elf_result)
if(NOT elf_result EQUAL 0)
    message(FATAL_ERROR
        "cuobjdump --list-elf failed for ${OKUFLOW_EXECUTABLE}: ${elf_error}")
endif()

string(REPLACE "," ";" expected_archs "${OKUFLOW_EXPECTED_ARCHS}")
set(missing_archs "")
foreach(arch IN LISTS expected_archs)
    if(NOT elf_list MATCHES "sm_${arch}\\.cubin")
        list(APPEND missing_archs "sm_${arch}")
    endif()
endforeach()

if(missing_archs)
    string(JOIN ", " missing_text ${missing_archs})
    message(FATAL_ERROR
        "${OKUFLOW_EXECUTABLE} is missing CUDA cubins: ${missing_text}\n"
        "Embedded images:\n${elf_list}")
endif()

message(STATUS
    "CUDA architecture coverage OK (${OKUFLOW_EXPECTED_ARCHS}) in "
    "${OKUFLOW_EXECUTABLE}")
