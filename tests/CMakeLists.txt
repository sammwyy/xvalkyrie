add_executable(xv_tests tests.cpp pe_directories.cpp loader_image.cpp xvd.cpp pri.cpp gameruntime.cpp dstorage.cpp memory.cpp
  console_shader.cpp)
target_link_libraries(xv_tests PRIVATE xv_options xv_pe xv_core xv_catch xv_xvd xv_pri xv_loader
  xv_runtime_common xv_runtime_modules d3dcompiler)
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
