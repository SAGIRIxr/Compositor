cmake_minimum_required(VERSION 3.21)
project(CompositorWindows VERSION 0.1.0 LANGUAGES C CXX)

# Compositor 的 Windows 版：C++ 核心库 + Qt6 界面。核心库不依赖 Qt，可在任何平台编译和测试。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(COMPOSITOR_BUILD_APP "构建 Qt6 桌面应用" ON)
option(COMPOSITOR_BUILD_TESTS "构建单元测试" ON)

find_package(Threads REQUIRED)

# 直接复用 macOS 版的 C 像素内核（纯标准 C，不复制源码）。
set(KERNEL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Compositor/Rendering)
add_library(compositor_kernels STATIC
    ${KERNEL_DIR}/AdjustPixels.c
    ${KERNEL_DIR}/BrushPixels.c
    ${KERNEL_DIR}/ContentFill.c
    ${KERNEL_DIR}/HealPixels.c
    ${KERNEL_DIR}/LensPixels.c
    ${KERNEL_DIR}/LevelsPixels.c
    ${KERNEL_DIR}/NoisePixels.c
    ${KERNEL_DIR}/WandPixels.c
)
target_include_directories(compositor_kernels PUBLIC ${KERNEL_DIR})
target_compile_definitions(compositor_kernels PRIVATE _USE_MATH_DEFINES _DEFAULT_SOURCE)
if(MSVC)
    target_compile_options(compositor_kernels PRIVATE /W1 /utf-8)
else()
    target_link_libraries(compositor_kernels PUBLIC m)
endif()

add_library(compositor_core STATIC
    core/src/adjustment.cpp
    core/src/blend.cpp
    core/src/document.cpp
    core/src/edit.cpp
    core/src/effects.cpp
    core/src/image.cpp
    core/src/image_io.cpp
    core/src/project_io.cpp
    core/src/renderer.cpp
    core/src/selection.cpp
    core/src/transform.cpp
    core/src/uuid.cpp
)
target_include_directories(compositor_core PUBLIC core/include third_party)
target_link_libraries(compositor_core PUBLIC compositor_kernels Threads::Threads)
if(MSVC)
    target_compile_options(compositor_core PUBLIC /utf-8 /permissive- /Zc:__cplusplus /bigobj)
    target_compile_definitions(compositor_core PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(compositor_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(compositor-cli cli/main.cpp)
target_link_libraries(compositor-cli PRIVATE compositor_core)
if(WIN32)
    target_link_libraries(compositor-cli PRIVATE shell32)
    install(TARGETS compositor-cli RUNTIME DESTINATION .)
endif()

if(COMPOSITOR_BUILD_TESTS)
    enable_testing()
    add_executable(compositor_tests tests/test_main.cpp tests/test_core.cpp)
    target_link_libraries(compositor_tests PRIVATE compositor_core)
    add_test(NAME compositor_tests COMMAND compositor_tests)
endif()

if(COMPOSITOR_BUILD_APP)
    add_subdirectory(app)
endif()
