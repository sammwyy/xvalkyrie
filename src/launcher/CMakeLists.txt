# What a title is and whether it can start, the game library and the saved configuration.
# No window and no console output: the CLI and the interface both sit on this.
add_library(xv_launcher launch.cpp config.cpp debug.cpp library.cpp update.cpp)
target_compile_definitions(xv_launcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xv_launcher PUBLIC xv_input nlohmann_json::nlohmann_json PRIVATE xv_options xv_graphics xv_host xv_appx xv_pri xv_game_config xv_pe xv_analysis xv_xvd xv_loader xv_runtime winhttp shell32)
