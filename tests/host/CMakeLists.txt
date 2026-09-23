cmake_minimum_required(VERSION 3.20)
project(launcher_test C)
set(CMAKE_C_STANDARD 11)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LVGL "${REPO}/SiFli-SDK/external/lvgl_v9")
file(GLOB_RECURSE LV_SOURCES CONFIGURE_DEPENDS "${LVGL}/src/*.c")
add_library(lvgl_test STATIC ${LV_SOURCES})
target_include_directories(lvgl_test PUBLIC "${LVGL}" "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(lvgl_test PUBLIC LV_KCONFIG_IGNORE LV_CONF_INCLUDE_SIMPLE
    _CRT_SECURE_NO_WARNINGS)
add_executable(launcher_test launcher_test.c "${REPO}/src/ui/launcher.c"
    "${REPO}/src/ui/ui_model.c" "${REPO}/src/ui/ui_font.c" "${REPO}/src/ui/icons/ui_icons.c"
    "${REPO}/src/ui/ui_weather_data.c" "${REPO}/src/ui/icons/weather_icons.c"
    "${REPO}/src/ui/ui_bookshelf_data.c" "${REPO}/src/ui/ui_settings.c")
target_include_directories(launcher_test PRIVATE "${REPO}/src/ui")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REPO}/project/proj.conf")
file(STRINGS "${REPO}/project/proj.conf" TINY_TTF_CACHE_CONFIG
    REGEX "^CONFIG_TINY_TTF_CACHE_SIZE=[0-9]+$")
if(NOT TINY_TTF_CACHE_CONFIG MATCHES "^CONFIG_TINY_TTF_CACHE_SIZE=([0-9]+)$")
    message(FATAL_ERROR "A single Tiny TTF bitmap heap size is required in project/proj.conf")
endif()
target_compile_definitions(launcher_test PRIVATE UI_TEST_TINY_TTF_CACHE_SIZE=${CMAKE_MATCH_1})
target_compile_definitions(launcher_test PRIVATE
    UI_BUILTIN_FONT_PATH="A:${REPO}/font/DroidSansFallback.ttf"
    UI_TEST_FONT_FILE="${REPO}/font/DroidSansFallback.ttf")
target_link_libraries(launcher_test PRIVATE lvgl_test)
if(MSVC)
    foreach(target lvgl_test launcher_test)
        target_compile_options(${target} PRIVATE /utf-8)
    endforeach()
endif()
enable_testing()
add_test(NAME launcher COMMAND launcher_test)
