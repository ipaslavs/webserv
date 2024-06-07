#ifndef SERVER_CONFIG_HPP
#define SERVER_CONFIG_HPP

#include <string>
#include <vector>
#include <map>

struct RouteConfig {
    std::string url;
    std::vector<std::string> methods;
    std::string root;
    std::string upload_dir;
    std::string cgi_path;
    std::string index;
    bool autoindex;
    std::string alias;
    size_t max_body;

    RouteConfig() : autoindex(false), max_body(0) {}
};

struct ServerConfig {
    int port;
    std::string server_name;
    std::map<int, std::string> error_pages;
    size_t client_max_body_size;
    std::vector<RouteConfig> routes;
};

std::vector<ServerConfig> parseConfigFile(const std::string &filename);
std::string trim(const std::string &str);

#endif // SERVER_CONFIG_HPP
