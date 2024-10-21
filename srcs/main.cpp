/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   main.cpp                                           :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: ipaslavs <marvin@42.fr>                    +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/10/21 16:31:34 by ipaslavs          #+#    #+#             */
/*   Updated: 2024/10/21 16:31:39 by ipaslavs         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include "WebServer.hpp"
#include "ServerConfig.hpp"
#include <iostream>

int main(int argc, char *argv[]) {
    std::string config_path = "config.yaml";

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
    } else if (argv[1]) {
        config_path = argv[1];
    }

    std::vector<ServerConfig> configs = parseConfigFile(config_path);
    if (configs.empty()) {
        std::cerr << "No valid server configurations found in the configuration file." << std::endl;
        return 1;
    }

    WebServer server(configs);
    server.run();

    return 0;
}
