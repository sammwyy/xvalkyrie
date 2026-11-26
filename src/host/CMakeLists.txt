add_library(xv_host STATIC host.cpp session_log.cpp)
target_include_directories(xv_host PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(xv_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xv_host PRIVATE xv_options shell32 ole32 uuid winmm pdh dxgi PUBLIC Threads::Threads)
