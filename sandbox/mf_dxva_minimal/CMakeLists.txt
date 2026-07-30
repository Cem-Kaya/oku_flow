add_executable(mf_dxva_minimal
    main.cpp
    interop_stress.cpp
    interop_stress.hpp
)

target_compile_features(mf_dxva_minimal PRIVATE cxx_std_20)
target_compile_definitions(mf_dxva_minimal PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    OPENZOOM_PROBE_HAS_CUDA=$<BOOL:${OPENZOOM_ENABLE_CUDA}>
)
target_link_libraries(mf_dxva_minimal PRIVATE
    d3d11
    d3d12
    dxgi
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
)

if(OPENZOOM_ENABLE_CUDA)
    target_link_libraries(mf_dxva_minimal PRIVATE
        CUDA::cudart_static
        CUDA::cuda_driver
    )
endif()
