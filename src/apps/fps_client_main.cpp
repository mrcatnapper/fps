#include <iostream>

#include "fps/net/tcp_relay_app.hpp"

auto main(int argc, char** argv) -> int { return fps::net::run_tcp_relay_cli(argc, argv, "--server", "server", fps::RelayRole::client, std::cout, std::cerr); }
