#include <iostream>

#include "fps/net/tcp_relay_app.hpp"

auto main(int argc, char** argv) -> int { return fps::net::run_tcp_relay_cli(argc, argv, "--origin", "origin", fps::RelayRole::server, std::cout, std::cerr); }
