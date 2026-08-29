# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# Host tests for clawd_motion nadenade, LED packing, and LCD/IMU face input.

cmake_minimum_required(VERSION 3.16)
project(clawd_motion_host_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(MOTION_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(REPO_ROOT ${MOTION_ROOT}/../..)

add_library(clawd_motion_host STATIC
    ${MOTION_ROOT}/motion.cpp
    ${MOTION_ROOT}/face_input.cpp
)
target_include_directories(clawd_motion_host
    PUBLIC ${MOTION_ROOT}/include
)
target_compile_options(clawd_motion_host PRIVATE -Wall -Wextra)

enable_testing()

add_executable(clawd_test_nadenade test_nadenade.cpp)
target_link_libraries(clawd_test_nadenade PRIVATE clawd_motion_host)
target_compile_options(clawd_test_nadenade PRIVATE -Wall -Wextra)
add_test(NAME nadenade COMMAND clawd_test_nadenade)

add_executable(clawd_test_led_runtime test_led_runtime.cpp)
target_include_directories(clawd_test_led_runtime PRIVATE ${REPO_ROOT}/main)
target_compile_options(clawd_test_led_runtime PRIVATE -Wall -Wextra)
add_test(NAME led_runtime COMMAND clawd_test_led_runtime)

add_executable(clawd_test_face_input test_face_input.cpp)
target_link_libraries(clawd_test_face_input PRIVATE clawd_motion_host)
target_compile_options(clawd_test_face_input PRIVATE -Wall -Wextra)
add_test(NAME face_input COMMAND clawd_test_face_input)
