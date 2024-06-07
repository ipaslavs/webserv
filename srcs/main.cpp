#include "WebServer.hpp"
#include <iostream>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<ServerConfig> configs = parseConfigFile(argv[1]);
    if (configs.empty()) {
        std::cerr << "No valid server configurations found in the configuration file." << std::endl;
        return EXIT_FAILURE;
    }

    WebServer server(configs);
    server.run();

    return 0;
}
