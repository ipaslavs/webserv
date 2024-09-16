// ServerConfig.hpp
#ifndef SERVERCONFIG_HPP
#define SERVERCONFIG_HPP

#include <vector>
#include <map>
#include <string>

struct RouteConfig {
    std::string url;
    std::vector<std::string> methods;
    std::string root;
    std::string index;
    bool autoindex;
    std::string alias;
    std::string cgi_path;
    std::string upload_dir;
    size_t max_body;
};

struct ServerConfig {
    int port;
    std::string server_name;
    size_t client_max_body_size;
    std::map<int, std::string> error_pages;
    std::vector<RouteConfig> routes;
};

std::vector<ServerConfig> parseConfigFile(const std::string &filename);

#endif // SERVERCONFIG_HPP
