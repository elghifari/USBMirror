set(USBMIRROR_CORE_SOURCE "${PROJECT_SOURCE_DIR}/src/Core/src")
add_executable(USBMirror.PreviewRotation.Tests
    "${CMAKE_CURRENT_LIST_DIR}/preview_rotation_test.cpp"
    "${USBMIRROR_CORE_SOURCE}/Renderer/D3D11PreviewRenderer.cpp"
    "${USBMIRROR_CORE_SOURCE}/Media/MediaFoundationDecoder.cpp"
    "${USBMIRROR_CORE_SOURCE}/Media/CoreMedia.cpp"
    "${USBMIRROR_CORE_SOURCE}/Media/H264.cpp"
    "${USBMIRROR_CORE_SOURCE}/Protocol/QuickTimePacket.cpp"
    "${USBMIRROR_CORE_SOURCE}/Logging.cpp")
target_include_directories(USBMirror.PreviewRotation.Tests PRIVATE "${USBMIRROR_CORE_SOURCE}")
target_compile_features(USBMirror.PreviewRotation.Tests PRIVATE cxx_std_20)
target_compile_definitions(USBMirror.PreviewRotation.Tests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(USBMirror.PreviewRotation.Tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
set_target_properties(USBMirror.PreviewRotation.Tests PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
target_link_libraries(USBMirror.PreviewRotation.Tests PRIVATE
    d3d11 d3dcompiler dcomp dxgi mfplat mf mfuuid wmcodecdspuuid
    ole32 user32 gdi32 dwmapi advapi32 bcrypt)
enable_testing()
add_test(NAME PreviewRotationRenderTests COMMAND USBMirror.PreviewRotation.Tests
    "${CMAKE_BINARY_DIR}/rotation-fixtures")
set_tests_properties(PreviewRotationRenderTests PROPERTIES TIMEOUT 20)
