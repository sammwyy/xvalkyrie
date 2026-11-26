include(FetchContent)
function(xv_dependency name repo version digest)
  FetchContent_Declare(${name}
    URL "https://codeload.github.com/${repo}/tar.gz/refs/tags/${version}"
    URL_HASH "SHA256=${digest}" SYSTEM ${ARGN})
  FetchContent_MakeAvailable(${name})
  set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_DOCS OFF)
set(GLFW_INSTALL OFF)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
xv_dependency(fmt fmtlib/fmt 11.1.4 ac366b7b4c2e9f0dde63a59b3feb5ee59b67974b14ee5dc9ea8ad78aa2c1ee1e)
xv_dependency(spdlog gabime/spdlog v1.15.3 15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67)
xv_dependency(json nlohmann/json v3.11.3 0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406)
set(PUGIXML_INSTALL OFF)
set(PUGIXML_NO_XPATH ON)
xv_dependency(pugixml zeux/pugixml v1.15 b39647064d9e28297a34278bfb897092bf33b7c487906ddfc094c9e8868bddcb)
xv_dependency(glfw glfw/glfw 3.4 c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01)
xv_dependency(imgui ocornut/imgui v1.91.9b 8e1bbc76c71d74fef2fb85db7e7ca8eba13d6a86623c54992b60162db554ffdb)
set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
xv_dependency(nfd btzy/nativefiledialog-extended v1.2.1 443697a857c4efacbe08cdaf5182724fa9d9b9a79b8feff2a1601bde1df46b07)
# libopus, a static library: the Series console's audio decoder (xapu.dll) takes Opus packets.
# A tag's archive has neither git nor the package_version file, so its version is given here.
# Its tests follow BUILD_TESTING, which stays on for the emulator's own.
block()
  set(BUILD_TESTING OFF)
  set(OPUS_BUILD_SHARED_LIBRARY OFF)
  set(OPUS_BUILD_TESTING OFF)
  set(OPUS_BUILD_PROGRAMS OFF)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
  set(OPUS_PACKAGE_VERSION 1.5.2)
  xv_dependency(opus xiph/opus v1.5.2 9480e329e989f70d69886ded470c7f8cfe6c0667cc4196d4837ac9e668fb7404)
endblock()
add_library(xv_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(xv_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(xv_imgui PRIVATE glfw)
if(BUILD_TESTING)
  xv_dependency(catch2 catchorg/Catch2 v2.13.10 d54a712b7b1d7708bc7a819a8e6e47b2fde9536f487b89ccbca295072a7d9943 SOURCE_SUBDIR single_include)
  add_library(xv_catch INTERFACE)
  target_include_directories(xv_catch SYSTEM INTERFACE ${catch2_SOURCE_DIR}/single_include)
endif()

# The DirectX 12 Agility SDK: Microsoft's newer Direct3D 12 runtime, for hosts whose own is
# older than what the later console kits ask of it. Only D3D12Core.dll is used, which the
# package's license lets an application carry; it goes next to xvboot (xv_deploy_agility).
# The tests run on it as well, with its debug layer, which is not shipped (LAYERS).
set(XV_AGILITY_VERSION 620)
FetchContent_Declare(xv_agility
  URL "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.620.2/microsoft.direct3d.d3d12.1.620.2.nupkg"
  URL_HASH "SHA256=70489669c2bbeaf318b62d72b0ac3e753d521387bfc848c9763791c4e920bfcc"
  DOWNLOAD_NAME agility.zip)
FetchContent_MakeAvailable(xv_agility)
function(xv_deploy_agility target)
  cmake_parse_arguments(PARSE_ARGV 1 deploy "LAYERS" "" "")
  target_compile_definitions(${target} PRIVATE XV_AGILITY_VERSION=${XV_AGILITY_VERSION})
  if(deploy_LAYERS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${xv_agility_SOURCE_DIR}/build/native/bin/x64/d3d12SDKLayers.dll"
        "$<TARGET_FILE_DIR:${target}>/D3D12/d3d12SDKLayers.dll"
      VERBATIM)
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${xv_agility_SOURCE_DIR}/build/native/bin/x64/D3D12Core.dll"
      "$<TARGET_FILE_DIR:${target}>/D3D12/D3D12Core.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${xv_agility_SOURCE_DIR}/LICENSE.txt"
      "$<TARGET_FILE_DIR:${target}>/D3D12/LICENSE.txt"
    VERBATIM)
endfunction()
