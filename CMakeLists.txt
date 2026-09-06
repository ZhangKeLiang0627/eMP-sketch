cmake_minimum_required(VERSION 3.16)

project(eMP-sketch VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# 顶层选项
# ---------------------------------------------------------------------------
# SKETCH_USE_SDL: ON = 桌面（LVGL SDL 后端，需 SDL2）；OFF = 嵌入式（fbdev + evdev）
# 交叉编译时自动切到嵌入式；桌面开发默认 SDL。
if(CMAKE_CROSSCOMPILING)
    set(_sketch_sdl_default OFF)
else()
    set(_sketch_sdl_default ON)
endif()
option(SKETCH_USE_SDL "Use LVGL SDL backend for desktop testing" ${_sketch_sdl_default})

set(SKETCH_BIN_NAME "eMP-sketch" CACHE STRING "Output executable name")
set(SKETCH_DEPS_DIR "${CMAKE_CURRENT_LIST_DIR}/dependencies" CACHE PATH "Third-party source directory")
set(SKETCH_OUTPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/dist" CACHE PATH "Runtime output directory")
set(SKETCH_WEB_ROOT "${CMAKE_CURRENT_LIST_DIR}/web" CACHE PATH "Web static asset directory (served over HTTP)")
set(SKETCH_WS_PORT "8080" CACHE STRING "HTTP/WebSocket listen port")

set(SKETCH_EVDEV_TOUCH "/dev/input/event1" CACHE STRING "evdev touch device path")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 路径变量
# ---------------------------------------------------------------------------
set(SKETCH_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(SKETCH_SRC_DIR "${SKETCH_ROOT}/src")
set(SKETCH_LVGL_DIR "${SKETCH_DEPS_DIR}/lvgl")
set(SKETCH_HTTPLIB_DIR "${SKETCH_DEPS_DIR}/httplib")
set(SKETCH_JSON_DIR "${SKETCH_DEPS_DIR}/json")

set(SKETCH_SYSTEM_LIBS ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
    list(APPEND SKETCH_SYSTEM_LIBS m)
endif()

# ---------------------------------------------------------------------------
# 依赖完整性检查
# ---------------------------------------------------------------------------
if(NOT EXISTS "${SKETCH_LVGL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "缺少依赖 lvgl：请先运行 ./bootstrap.sh")
endif()
if(NOT EXISTS "${SKETCH_HTTPLIB_DIR}/httplib.h")
    message(FATAL_ERROR "缺少依赖 cpp-httplib：请先运行 ./bootstrap.sh")
endif()
if(NOT EXISTS "${SKETCH_JSON_DIR}/single_include/nlohmann/json.hpp")
    message(FATAL_ERROR "缺少依赖 nlohmann/json：请先运行 ./bootstrap.sh")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# LVGL（关闭示例/测试/bench，仅编译库本体）
# ---------------------------------------------------------------------------
set(LV_BUILD_CONF_PATH "${SKETCH_ROOT}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

add_subdirectory("${SKETCH_LVGL_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dependencies/lvgl" EXCLUDE_FROM_ALL)

# 告诉 lv_conf.h 当前后端：SDL 还是 fbdev+evdev
target_compile_definitions(lvgl PUBLIC
    SKETCH_LVGL_USE_SDL=$<IF:$<BOOL:${SKETCH_USE_SDL}>,1,0>
    SKETCH_LVGL_USE_EVDEV=$<IF:$<BOOL:${SKETCH_USE_SDL}>,0,1>
)

# ---------------------------------------------------------------------------
# SDL2（仅桌面模式）
# ---------------------------------------------------------------------------
set(SKETCH_SDL_LIBS "")
if(SKETCH_USE_SDL)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
    endif()
    if(SDL2_FOUND)
        target_link_libraries(lvgl PRIVATE PkgConfig::SDL2)
        set(SKETCH_SDL_LIBS PkgConfig::SDL2)
    else()
        find_package(SDL2 CONFIG QUIET)
        if(TARGET SDL2::SDL2)
            target_link_libraries(lvgl PRIVATE SDL2::SDL2)
            set(SKETCH_SDL_LIBS SDL2::SDL2)
        else()
            find_package(SDL2 REQUIRED)
            target_include_directories(lvgl SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
            set(SKETCH_SDL_LIBS ${SDL2_LIBRARIES})
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# 本工程可执行文件
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SKETCH_SOURCES CONFIGURE_DEPENDS
    "${SKETCH_SRC_DIR}/*.cpp"
)
file(GLOB_RECURSE SKETCH_HEADERS CONFIGURE_DEPENDS
    "${SKETCH_SRC_DIR}/*.hpp"
)

add_executable(${SKETCH_BIN_NAME}
    ${SKETCH_SOURCES}
    ${SKETCH_HEADERS}
)

target_include_directories(${SKETCH_BIN_NAME} PRIVATE
    "${SKETCH_SRC_DIR}"
    "${SKETCH_HTTPLIB_DIR}"
    "${SKETCH_JSON_DIR}/single_include"
    "${SKETCH_JSON_DIR}/include"
)

target_compile_definitions(${SKETCH_BIN_NAME} PRIVATE
    SKETCH_USE_SDL=$<IF:$<BOOL:${SKETCH_USE_SDL}>,1,0>
    SKETCH_WEB_ROOT="${SKETCH_WEB_ROOT}"
    SKETCH_WS_PORT="${SKETCH_WS_PORT}"
    SKETCH_EVDEV_TOUCH="${SKETCH_EVDEV_TOUCH}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${SKETCH_BIN_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(${SKETCH_BIN_NAME} PRIVATE
    lvgl::lvgl
    Threads::Threads
    ${SKETCH_SDL_LIBS}
    ${SKETCH_SYSTEM_LIBS}
)

if(WIN32)
    target_link_libraries(${SKETCH_BIN_NAME} PRIVATE ws2_32 crypt32)
endif()

set_target_properties(${SKETCH_BIN_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SKETCH_OUTPUT_DIR}"
)
