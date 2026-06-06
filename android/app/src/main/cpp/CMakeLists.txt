cmake_minimum_required(VERSION 3.22.1)

project(fps_android_native LANGUAGES CXX)

get_filename_component(FPS_REPO_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../.."
  ABSOLUTE)

set(FPS_ANDROID_BOOST_DIR "/usr/include/boost" CACHE PATH
  "Path to the Boost header directory for Android header-only Boost use")

set(FPS_ANDROID_VCPKG_ROOT "/opt/vcpkg" CACHE PATH
  "Path to the vcpkg root used only for Android OpenSSL")

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

if(ANDROID_ABI STREQUAL "arm64-v8a")
  set(FPS_ANDROID_VCPKG_TRIPLET "arm64-android")
elseif(ANDROID_ABI STREQUAL "x86_64")
  set(FPS_ANDROID_VCPKG_TRIPLET "x64-android")
else()
  message(FATAL_ERROR "Unsupported Android ABI for FPS native smoke: ${ANDROID_ABI}")
endif()

set(FPS_ANDROID_VCPKG_INSTALLED
  "${FPS_ANDROID_VCPKG_ROOT}/installed/${FPS_ANDROID_VCPKG_TRIPLET}")
set(FPS_ANDROID_OPENSSL_INCLUDE_DIR
  "${FPS_ANDROID_VCPKG_INSTALLED}/include")
set(FPS_ANDROID_CRYPTO_LIBRARY
  "${FPS_ANDROID_VCPKG_INSTALLED}/lib/libcrypto.a")

if(NOT EXISTS "${FPS_ANDROID_OPENSSL_INCLUDE_DIR}/openssl/evp.h")
  message(FATAL_ERROR
    "Android OpenSSL headers not found. Install with: "
    "ANDROID_NDK_HOME=<ndk> ${FPS_ANDROID_VCPKG_ROOT}/vcpkg install "
    "openssl:${FPS_ANDROID_VCPKG_TRIPLET}")
endif()

if(NOT EXISTS "${FPS_ANDROID_CRYPTO_LIBRARY}")
  message(FATAL_ERROR
    "Android libcrypto not found: ${FPS_ANDROID_CRYPTO_LIBRARY}")
endif()

set(FPS_ANDROID_CORE_SMOKE_SOURCES
  "${FPS_REPO_ROOT}/src/core/cover_session_pipeline.cpp"
  "${FPS_REPO_ROOT}/src/core/covert_codec.cpp"
  "${FPS_REPO_ROOT}/src/core/crypto.cpp"
  "${FPS_REPO_ROOT}/src/core/fps_classified_record.cpp"
  "${FPS_REPO_ROOT}/src/core/fps_envelope.cpp"
  "${FPS_REPO_ROOT}/src/core/fps_envelope_pipeline.cpp"
  "${FPS_REPO_ROOT}/src/core/fps_upgrade_controller.cpp"
  "${FPS_REPO_ROOT}/src/core/identity.cpp"
  "${FPS_REPO_ROOT}/src/core/shaper.cpp"
  "${FPS_REPO_ROOT}/src/core/tls_record_layer.cpp"
  "${FPS_REPO_ROOT}/src/core/tls_record_parser.cpp"
  "${FPS_REPO_ROOT}/src/core/zero_rtt_upgrade.cpp"
  "${FPS_REPO_ROOT}/src/net/client_upgrade_delay.cpp"
  "${FPS_REPO_ROOT}/src/net/covert_datagram_transport.cpp"
  "${FPS_REPO_ROOT}/src/net/tcp_socket_options.cpp"
  "${FPS_REPO_ROOT}/src/net/tcp_socket_protector.cpp"
  "${FPS_REPO_ROOT}/src/net/tls_tcp_carrier_adapter.cpp"
  "${FPS_REPO_ROOT}/src/net/tls_tcp_carrier_session.cpp"
  "${FPS_REPO_ROOT}/src/net/tls_tcp_carrier_session_io.cpp"
  "${FPS_REPO_ROOT}/src/net/tun_packet.cpp")

add_library(fps_android_core_smoke STATIC
  ${FPS_ANDROID_CORE_SMOKE_SOURCES})

target_include_directories(fps_android_core_smoke
  SYSTEM PRIVATE
    "${FPS_ANDROID_BOOST_INCLUDE_ROOT}"
    "${FPS_ANDROID_OPENSSL_INCLUDE_DIR}"
  PRIVATE
    "${FPS_REPO_ROOT}/include")

target_compile_features(fps_android_core_smoke PUBLIC cxx_std_20)

target_compile_definitions(fps_android_core_smoke PUBLIC
  BOOST_ERROR_CODE_HEADER_ONLY
  BOOST_SYSTEM_NO_DEPRECATED)

target_compile_options(fps_android_core_smoke PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion)

target_link_libraries(fps_android_core_smoke PUBLIC
  "${FPS_ANDROID_CRYPTO_LIBRARY}")

add_library(fps_android_native SHARED
  fps_android_native.cpp)

target_include_directories(fps_android_native
  SYSTEM PRIVATE
    "${FPS_ANDROID_BOOST_INCLUDE_ROOT}"
    "${FPS_ANDROID_OPENSSL_INCLUDE_DIR}"
  PRIVATE
    "${FPS_REPO_ROOT}/include")

target_compile_features(fps_android_native PRIVATE cxx_std_20)

target_compile_definitions(fps_android_native PRIVATE
  BOOST_ERROR_CODE_HEADER_ONLY
  BOOST_SYSTEM_NO_DEPRECATED)

target_compile_options(fps_android_native PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion)

find_library(log_lib log)
target_link_libraries(fps_android_native PRIVATE
  fps_android_core_smoke
  ${log_lib})
