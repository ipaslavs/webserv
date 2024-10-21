/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   ServerConfig.cpp                                   :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: ipaslavs <marvin@42.fr>                    +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/10/21 16:27:42 by ipaslavs          #+#    #+#             */
/*   Updated: 2024/10/21 16:29:43 by ipaslavs         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */


#include "ServerConfig.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>

std::string trim(const std::string &str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    size_t end = str.find_last_not_of(" \t\n\r");
    return (start == std::string::npos || end == std::string::npos) ? "" : str.substr(start, end - start + 1);
}

std::vector<ServerConfig> parseConfigFile(const std::string &filename) {
    std::vector<ServerConfig> configs;
    std::ifstream configFile(filename.c_str());
    std::string line;

    if (!configFile.is_open()) {
        std::cerr << "Could not open configuration file: " << filename << std::endl;
        exit(EXIT_FAILURE);
    }

    ServerConfig config;
    RouteConfig route;
    route.url = ""; // Initialize to empty string
    route.methods = std::vector<std::string>();
    route.root = "";
    route.index = "index.html";
    route.autoindex = false; // Set default to 'off'
    route.alias = "";
    route.upload_dir = "";
    route.cgi_path = "";
    route.max_body = 0;
    bool in_server_block = false;
    bool in_error_pages_block = false;
    bool in_routes_block = false;

    while (std::getline(configFile, line)) {
        std::istringstream iss(line);
        std::string key;

        if (line.find("servers:") != std::string::npos) {
            continue;
        }

        if (line.find("- port:") != std::string::npos) {
            if (in_server_block) {
                if (!route.url.empty()) {
                    config.routes.push_back(route);
                }
                configs.push_back(config);
                config = ServerConfig();
                route = RouteConfig();
            }
            in_server_block = true;
            in_error_pages_block = false;
            in_routes_block = false;
            std::string port_str = line.substr(line.find(":") + 1);
            std::istringstream(port_str) >> config.port;
            std::cout << "Parsed port: " << config.port << std::endl;
        } else if (in_server_block && line.find("server_name:") != std::string::npos) {
            iss >> key >> config.server_name;
            std::cout << "Parsed server_name: " << config.server_name << std::endl;
        } else if (in_server_block && line.find("client_max_body_size:") != std::string::npos) {
            iss >> key >> config.client_max_body_size;
            std::cout << "Parsed client_max_body_size: " << config.client_max_body_size << std::endl;
        } else if (in_server_block && line.find("error_pages:") != std::string::npos) {
            in_error_pages_block = true;
            in_routes_block = false;
        } else if (in_server_block && line.find("routes:") != std::string::npos) {
            in_routes_block = true;
            in_error_pages_block = false;
        } else if (in_error_pages_block) {
            int error_code;
            std::string error_page;
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                error_code = std::atoi(line.substr(0, pos).c_str());
                error_page = trim(line.substr(pos + 1));
                config.error_pages[error_code] = error_page;
                std::cout << "Parsed error_page: " << error_code << " -> " << error_page << std::endl;
            }
        } else if (in_routes_block) {
            if (line.find("url:") != std::string::npos) {
                if (!route.url.empty()) {
                    config.routes.push_back(route);
                    route = RouteConfig();
                }
                route.url = trim(line.substr(line.find(":") + 1));
                std::cout << "Parsed route url: " << route.url << std::endl;
            } else if (line.find("methods:") != std::string::npos) {
                std::string methods_line = line.substr(line.find("[") + 1, line.find("]") - line.find("[") - 1);
                std::istringstream methods_stream(methods_line);
                std::string method;
                while (std::getline(methods_stream, method, ',')) {
                    route.methods.push_back(trim(method));
                }
                std::cout << "Parsed route methods: ";
                for (size_t i = 0; i < route.methods.size(); ++i) {
                    std::cout << route.methods[i];
                    if (i < route.methods.size() - 1) {
                        std::cout << ", ";
                    }
                }
                std::cout << std::endl;
            } else if (line.find("root:") != std::string::npos) {
                route.root = trim(line.substr(line.find(":") + 1));
                std::cout << "Parsed route root: " << route.root << std::endl;
            } else if (line.find("upload_dir:") != std::string::npos) {
                route.upload_dir = trim(line.substr(line.find(":") + 1));
                std::cout << "Parsed route upload_dir: " << route.upload_dir << std::endl;
            } else if (line.find("cgi_path:") != std::string::npos) {
                route.cgi_path = trim(line.substr(line.find(":") + 1));
                std::cout << "Parsed route cgi_path: " << route.cgi_path << std::endl;
            } else if (line.find("index:") != std::string::npos) {
                route.index = trim(line.substr(line.find(":") + 1));
                std::cout << "Parsed route index: " << route.index << std::endl;
            } else if (line.find("autoIndex:") != std::string::npos) {
                std::string autoIndex_str = trim(line.substr(line.find(":") + 1));
                route.autoindex = (autoIndex_str == "on");
                std::cout << "Parsed route autoIndex: " << (route.autoindex ? "on" : "off") << std::endl;
            } else if (line.find("alias:") != std::string::npos) {
                route.alias = trim(line.substr(line.find(":") + 1));
                std::cout << "Parsed route alias: " << route.alias << std::endl;
            } else if (line.find("max_body:") != std::string::npos) {
                std::string max_body_str = trim(line.substr(line.find(":") + 1));
                route.max_body = std::atoi(max_body_str.c_str());
                std::cout << "Parsed route max_body: " << route.max_body << std::endl;
            }
        }
    }

    if (in_server_block) {
        if (!route.url.empty()) {
            config.routes.push_back(route);
        }
        configs.push_back(config);
    }

    for (size_t i = 0; i < configs.size(); ++i) {
        const ServerConfig &cfg = configs[i];
        std::cout << "Server on port: " << cfg.port << std::endl;
        for (size_t j = 0; j < cfg.routes.size(); ++j) {
            const RouteConfig &route = cfg.routes[j];
            std::cout << "  Route " << j << ": URL = '" << route.url << "', Methods = [";
            for (size_t k = 0; k < route.methods.size(); ++k) {
                std::cout << route.methods[k];
                if (k < route.methods.size() - 1) {
                    std::cout << ", ";
                }
            }
            std::cout << "], Root = '" << route.root << "', Index = '" << route.index << "', AutoIndex = '" << (route.autoindex ? "on" : "off") << "', Alias = '" << route.alias << "', Max Body = '" << route.max_body << "'" << std::endl;
        }
    }

    return configs;
}
