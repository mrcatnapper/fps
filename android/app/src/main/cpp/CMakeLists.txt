cmake_minimum_required(VERSION 3.22.1)

project(fps_android_native LANGUAGES CXX)

get_filename_component(FPS_REPO_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../.."
  ABSOLUTE)

add_library(fps_android_native SHARED
  fps_android_native.cpp
  "${FPS_REPO_ROOT}/src/net/tun_packet.cpp")

set(FPS_ANDROID_BOOST_DIR "/usr/include/boost" CACHE PATH
  "Path to the Boost header directory for Android header-only Boost use")

if(NOT EXISTS "${FPS_ANDROID_BOOST_DIR}/describe.hpp")
  message(FATAL_ERROR
    "FPS_ANDROID_BOOST_DIR must point to a Boost header directory; "
    "expected ${FPS_ANDROID_BOOST_DIR}/describe.hpp")
endif()

set(FPS_ANDROID_BOOST_INCLUDE_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/fps_boost_include")
file(MAKE_DIRECTORY "${FPS_ANDROID_BOOST_INCLUDE_ROOT}")
file(REMOVE_RECURSE "${FPS_ANDROID_BOOST_INCLUDE_ROOT}/boost")
file(CREATE_LINK
  "${FPS_ANDROID_BOOST_DIR}"
  "${FPS_ANDROID_BOOST_INCLUDE_ROOT}/boost"
  SYMBOLIC COPY_ON_ERROR)

target_include_directories(fps_android_native
  SYSTEM PRIVATE
    "${FPS_ANDROID_BOOST_INCLUDE_ROOT}"
  PRIVATE
    "${FPS_REPO_ROOT}/include")

target_compile_features(fps_android_native PRIVATE cxx_std_20)

target_compile_options(fps_android_native PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion)

find_library(log_lib log)
target_link_libraries(fps_android_native PRIVATE ${log_lib})
