cmake_minimum_required(VERSION 3.24)

project(fps
  VERSION 0.1.0
  DESCRIPTION "FPS / Free Porn Storage v2 relay"
  LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug
    Release
    RelWithDebInfo
    MinSizeRel)
endif()

option(FPS_BUILD_TESTS "Build FPS unit tests" ON)
option(FPS_ENABLE_TUN_TESTS "Enable opt-in Linux TUN integration tests" OFF)
option(FPS_ENABLE_PCAP_TESTS "Enable opt-in pcap/TLS wire-shape integration tests" OFF)
option(FPS_BUILD_FUZZERS "Build clang/libFuzzer fuzz targets" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING
    "Flags used by the CXX compiler during Release builds." FORCE)
endif()

include(GNUInstallDirs)

find_package(Boost 1.83 REQUIRED COMPONENTS
  filesystem
  json
  log
  log_setup
  program_options
  system
  thread
  unit_test_framework)
find_package(OpenSSL REQUIRED)
find_package(Python3 COMPONENTS Interpreter)

add_library(fps_core
  src/core/cover_session_pipeline.cpp
  src/core/covert_codec.cpp
  src/core/crypto.cpp
  src/core/fps_classified_record.cpp
  src/core/fps_envelope.cpp
  src/core/fps_envelope_pipeline.cpp
  src/core/fps_upgrade_controller.cpp
  src/core/identity.cpp
  src/core/shaper.cpp
  src/core/tls_record_layer.cpp
  src/core/tls_record_parser.cpp
  src/core/zero_rtt_upgrade.cpp
  src/log/logging.cpp
  src/net/covert_datagram_transport.cpp
  src/net/tcp_bridge_session.cpp
  src/net/tcp_bridge_session_io.cpp
  src/net/tun_lease.cpp
  src/net/tun_runtime.cpp
  src/net/tun_tunnel_adapter.cpp
  src/net/tun_packet_pump.cpp)

target_include_directories(fps_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(fps_core
  PUBLIC
    Boost::boost
    Boost::system
  PRIVATE
    Boost::filesystem
    Boost::json
    Boost::log
    Boost::log_setup
    Boost::thread
    OpenSSL::Crypto)

target_compile_definitions(fps_core PUBLIC BOOST_LOG_DYN_LINK)

target_compile_features(fps_core PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fps_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
endif()

if(FPS_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "FPS_BUILD_FUZZERS requires Clang/libFuzzer")
  endif()

  function(fps_add_fuzzer target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE fps_core)
    target_compile_options(${target_name} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion)
    target_link_options(${target_name} PRIVATE
      -fsanitize=fuzzer,address,undefined)
  endfunction()

  fps_add_fuzzer(fps_fuzz_tls_records tests/fuzz/fuzz_tls_records.cpp)
  fps_add_fuzzer(fps_fuzz_covert_codec tests/fuzz/fuzz_covert_codec.cpp)
  fps_add_fuzzer(fps_fuzz_envelope tests/fuzz/fuzz_envelope.cpp)
  fps_add_fuzzer(fps_fuzz_zero_rtt tests/fuzz/fuzz_zero_rtt.cpp)
  fps_add_fuzzer(fps_fuzz_tun_frames tests/fuzz/fuzz_tun_frames.cpp)
endif()

add_library(fps_linux_runtime
  src/net/tcp_relay_config.cpp
  src/net/tcp_relay_config_shaper.cpp
  src/net/tcp_relay_app.cpp
  src/net/tcp_relay_cli.cpp
  src/platform/linux/tun_device.cpp
  src/platform/linux/tun_runtime.cpp)

target_include_directories(fps_linux_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(fps_linux_runtime
  PUBLIC
    fps_core
  PRIVATE
    Boost::filesystem
    Boost::json
    Boost::program_options)

target_compile_features(fps_linux_runtime PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fps_linux_runtime PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion)
endif()

add_executable(fps_client src/apps/fps_client_main.cpp)
target_link_libraries(fps_client PRIVATE fps_linux_runtime)

add_executable(fps_server src/apps/fps_server_main.cpp)
target_link_libraries(fps_server PRIVATE fps_linux_runtime)

install(TARGETS fps_client fps_server
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(PROGRAMS tools/fps_linux_route.sh
  DESTINATION ${CMAKE_INSTALL_BINDIR})

install(PROGRAMS tools/fps_carrier.py
  DESTINATION ${CMAKE_INSTALL_BINDIR}
  RENAME fps_carrier)

if(FPS_BUILD_TESTS)
  enable_testing()

  function(fps_set_test_properties test_name timeout labels)
    set_tests_properties(${test_name}
      PROPERTIES
        TIMEOUT ${timeout}
        LABELS "${labels}")
  endfunction()

  add_executable(fps_unit_tests
    tests/test_main.cpp
    tests/test_cover_session_pipeline.cpp
    tests/test_covert_codec.cpp
    tests/test_crypto.cpp
    tests/test_enum.cpp
    tests/test_fps_classified_record.cpp
    tests/test_fps_envelope.cpp
    tests/test_fps_envelope_pipeline.cpp
    tests/test_fps_upgrade_controller.cpp
    tests/test_identity.cpp
    tests/test_logging.cpp
    tests/test_covert_datagram_transport.cpp
    tests/test_tun_tunnel_adapter.cpp
    tests/test_shaper.cpp
    tests/test_tcp_bridge_session.cpp
    tests/test_tcp_relay_app.cpp
    tests/test_tls_record_layer.cpp
    tests/test_tls_record_parser.cpp
    tests/test_tun_lease.cpp
    tests/test_tun_packet_pump.cpp
    tests/test_wire.cpp
    tests/test_zero_rtt_upgrade.cpp)

  target_link_libraries(fps_unit_tests
    PRIVATE
      fps_core
      fps_linux_runtime
      Boost::unit_test_framework)

  target_compile_definitions(fps_unit_tests PRIVATE BOOST_TEST_DYN_LINK)

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fps_unit_tests PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion)
  endif()

  add_test(NAME fps_unit_tests COMMAND fps_unit_tests)
  fps_set_test_properties(fps_unit_tests 10 "unit;local")

  if(Python3_Interpreter_FOUND)
    add_test(NAME fps_cli_streams
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_streams.py
        --fps-client $<TARGET_FILE:fps_client>
        --fps-server $<TARGET_FILE:fps_server>)
    fps_set_test_properties(fps_cli_streams 10 "integration;local")

    add_test(NAME fps_linux_route_helper
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/linux_route_helper.py
        --script ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_linux_route.sh)
    fps_set_test_properties(fps_linux_route_helper 10 "integration;local;ops")

    add_test(NAME fps_status_socket
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/status_socket.py
        --fps-server $<TARGET_FILE:fps_server>)
    fps_set_test_properties(fps_status_socket 10 "integration;local;ops")

    add_test(NAME fps_carrier_tool
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/carrier_tool.py
        --carrier ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_carrier.py)
    fps_set_test_properties(fps_carrier_tool 15 "integration;local;wss")

    add_test(NAME fps_docker_artifacts
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/docker_artifacts.py
        --repo ${CMAKE_CURRENT_SOURCE_DIR})
    fps_set_test_properties(fps_docker_artifacts 10 "integration;local;ops;docker")

    add_test(NAME fps_https_passthrough
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/https_passthrough.py
        --fps-server $<TARGET_FILE:fps_server>)
    fps_set_test_properties(fps_https_passthrough 15 "integration;local")

    add_test(NAME fps_https_zero_rtt_chain
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/https_zero_rtt_chain.py
        --fps-client $<TARGET_FILE:fps_client>
        --fps-server $<TARGET_FILE:fps_server>)
    fps_set_test_properties(fps_https_zero_rtt_chain 20 "integration;local;zero_rtt")

    add_test(NAME fps_https_zero_rtt_hint_precheck
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/https_zero_rtt_chain.py
        --fps-client $<TARGET_FILE:fps_client>
        --fps-server $<TARGET_FILE:fps_server>
        --decoy-allowed-clients 2)
    fps_set_test_properties(fps_https_zero_rtt_hint_precheck 20 "integration;local;zero_rtt")

    add_test(NAME fps_https_zero_rtt_large_response
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/https_zero_rtt_large_response.py
        --fps-client $<TARGET_FILE:fps_client>
        --fps-server $<TARGET_FILE:fps_server>)
    fps_set_test_properties(fps_https_zero_rtt_large_response 20 "integration;local;zero_rtt")

    add_test(NAME fps_https_zero_rtt_multi_session
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/https_zero_rtt_multi_session.py
        --fps-client $<TARGET_FILE:fps_client>
        --fps-server $<TARGET_FILE:fps_server>)
    fps_set_test_properties(fps_https_zero_rtt_multi_session 25 "integration;local;zero_rtt")

    add_test(NAME fps_https_zero_rtt_adversarial
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/https_zero_rtt_adversarial.py
        --fps-client $<TARGET_FILE:fps_client>
        --fps-server $<TARGET_FILE:fps_server>)
    fps_set_test_properties(fps_https_zero_rtt_adversarial 30 "integration;local;zero_rtt")

    add_test(NAME fps_wss_passthrough
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/wss_passthrough.py
        --fps-server $<TARGET_FILE:fps_server>
        --carrier ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_carrier.py)
    fps_set_test_properties(fps_wss_passthrough 15 "integration;local;wss")

    add_test(NAME fps_wss_zero_rtt_chain
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/wss_zero_rtt_chain.py
        --fps-client $<TARGET_FILE:fps_client>
        --fps-server $<TARGET_FILE:fps_server>
        --carrier ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_carrier.py)
    fps_set_test_properties(fps_wss_zero_rtt_chain 25 "integration;local;wss;zero_rtt")

    add_test(NAME fps_carrier_http_zero_rtt
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/carrier_http_zero_rtt.py
        --fps-client $<TARGET_FILE:fps_client>
        --fps-server $<TARGET_FILE:fps_server>
        --carrier ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_carrier.py)
    fps_set_test_properties(fps_carrier_http_zero_rtt 25 "integration;local;wss;zero_rtt")

    if(FPS_ENABLE_PCAP_TESTS)
      add_test(NAME fps_pcap_tls_shape
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/pcap_tls_shape.py
          --fps-client $<TARGET_FILE:fps_client>
          --fps-server $<TARGET_FILE:fps_server>)
      set_tests_properties(fps_pcap_tls_shape
        PROPERTIES
          SKIP_RETURN_CODE 77
          TIMEOUT 30
          LABELS "integration;pcap;sudo;zero_rtt")
    endif()

    if(FPS_ENABLE_TUN_TESTS)
      add_test(NAME fps_tun_open_smoke
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tun_open_smoke.py
          --fps-server $<TARGET_FILE:fps_server>)
      set_tests_properties(fps_tun_open_smoke
        PROPERTIES
          SKIP_RETURN_CODE 77
          LABELS "integration;tun;sudo"
          TIMEOUT 10)

      add_test(NAME fps_tun_zero_rtt_loopback
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tun_zero_rtt_loopback.py
          --fps-client $<TARGET_FILE:fps_client>
          --fps-server $<TARGET_FILE:fps_server>
          --carrier ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_carrier.py
          --expect-carrier-count 1)
      set_tests_properties(fps_tun_zero_rtt_loopback
        PROPERTIES
          SKIP_RETURN_CODE 77
          LABELS "integration;tun;sudo;zero_rtt"
          TIMEOUT 60)

      add_test(NAME fps_tun_zero_rtt_burst
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tun_zero_rtt_loopback.py
          --fps-client $<TARGET_FILE:fps_client>
          --fps-server $<TARGET_FILE:fps_server>
          --carrier ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_carrier.py
          --expect-carrier-count 1
          --udp-count 64
          --udp-payload-size 1200)
      set_tests_properties(fps_tun_zero_rtt_burst
        PROPERTIES
          SKIP_RETURN_CODE 77
          LABELS "integration;tun;sudo;zero_rtt"
          TIMEOUT 90)

      add_test(NAME fps_tun_zero_rtt_fragmentation
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tun_zero_rtt_loopback.py
          --fps-client $<TARGET_FILE:fps_client>
          --fps-server $<TARGET_FILE:fps_server>
          --carrier ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_carrier.py
          --expect-carrier-count 1
          --udp-count 8
          --udp-payload-size 1200
          --tun-mtu 1280
          --codec-max-frame-payload 400)
      set_tests_properties(fps_tun_zero_rtt_fragmentation
        PROPERTIES
          SKIP_RETURN_CODE 77
          LABELS "integration;tun;sudo;zero_rtt;fragmentation"
          TIMEOUT 90)

      add_test(NAME fps_tun_zero_rtt_shaper
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tun_zero_rtt_loopback.py
          --fps-client $<TARGET_FILE:fps_client>
          --fps-server $<TARGET_FILE:fps_server>
          --carrier ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_carrier.py
          --expect-carrier-count 1
          --udp-count 1
          --udp-payload-size 1200
          --tun-mtu 1280
          --codec-max-frame-payload 400
          --cover-padding-size 4096
          --enable-shaper)
      set_tests_properties(fps_tun_zero_rtt_shaper
        PROPERTIES
          SKIP_RETURN_CODE 77
          LABELS "integration;tun;sudo;zero_rtt;shaper;fragmentation"
          TIMEOUT 90)

      add_test(NAME fps_tun_zero_rtt_multi_carrier
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tun_zero_rtt_loopback.py
          --fps-client $<TARGET_FILE:fps_client>
          --fps-server $<TARGET_FILE:fps_server>
          --carrier ${CMAKE_CURRENT_SOURCE_DIR}/tools/fps_carrier.py
          --carrier-count 2
          --expect-carrier-count 2
          --udp-count 16
          --udp-payload-size 512)
      set_tests_properties(fps_tun_zero_rtt_multi_carrier
        PROPERTIES
          SKIP_RETURN_CODE 77
          LABELS "integration;tun;sudo;zero_rtt;multi_carrier"
          TIMEOUT 90)
    endif()
  endif()
endif()
