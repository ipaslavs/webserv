#ifndef ROUTECONFIG_HPP
#define ROUTECONFIG_HPP

#include <string>
#include <vector>

class RouteConfig {
public:
    RouteConfig();
    std::string url;
    std::vector<std::string> methods;
    std::string root;
    std::string upload_dir;
    std::string cgi_path;
    std::string index;
    bool autoindex;
    std::string alias;
    size_t max_body;
};

#endif // ROUTECONFIG_HPP
