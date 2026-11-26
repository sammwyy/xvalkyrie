add_executable(xv_tests tests.cpp pe_directories.cpp loader_image.cpp xvd.cpp pri.cpp
  gameruntime.cpp dstorage.cpp console_shader.cpp memory.cpp timers.cpp placement.cpp
  frame_events.cpp d3d12x.cpp d3d12x_memory.cpp d3d12x_formats.cpp d3d12x_objects.cpp xg.cpp
  d3d12x_descriptors.cpp d3d12x_queues.cpp xapu.cpp console_model.cpp)
target_link_libraries(xv_tests PRIVATE xv_options xv_pe xv_core xv_catch xv_xvd xv_pri xv_loader
  xv_runtime_common xv_runtime_modules opus d3dcompiler d3d12 dxgi)
# Catch2's own handler of faults takes as fatal those the code catches itself (see tests.cpp)
target_compile_definitions(xv_tests PRIVATE CATCH_CONFIG_NO_WINDOWS_SEH)
# the Direct3D 12 runtime xvboot runs titles on, whatever the host's is
xv_deploy_agility(xv_tests LAYERS)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME fixture COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/corpus/generate.py"
  --all "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(fixture PROPERTIES FIXTURES_SETUP pe_file)
add_test(NAME parsers COMMAND xv_tests)
set_tests_properties(parsers PROPERTIES FIXTURES_REQUIRED pe_file)
if(XV_VULKAN)
  add_test(NAME vulkan_readback COMMAND xvalkyrie-vk-check)
  set_tests_properties(vulkan_readback PROPERTIES TIMEOUT 30 FAIL_REGULAR_EXPRESSION "Validation Error|VUID-")
endif()
