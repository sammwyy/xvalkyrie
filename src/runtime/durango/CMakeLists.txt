add_library(xv_runtime_durango runtime.cpp kernelx.cpp system.cpp winrt/activation.cpp
  winrt/placeholder.cpp winrt/inert.cpp winrt/application.cpp winrt/async.cpp winrt/collection.cpp winrt/gamepad.cpp winrt/storage.cpp winrt/system_ui.cpp winrt/title_info.cpp winrt/networking.cpp
  d3d11x/device.cpp d3d11x/wrap.cpp d3d11x/layout.cpp d3d11x/descriptors.cpp)
target_include_directories(xv_runtime_durango PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(xv_runtime_durango PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xv_runtime_durango PRIVATE xv_options xv_appx xv_host xv_runtime_common xv_runtime_modules xv_graphics xv_input xv_surface
  xv_graphics_d3d11 xv_graphics_dxvk runtimeobject d3d11 dxgi shlwapi onecore ws2_32)
