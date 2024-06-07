#ifndef WEB_SERVER_HPP
#define WEB_SERVER_HPP

#include "ServerConfig.hpp"
#include <vector>
#include <string>
#include <map>
#include <poll.h>

#define MAX_CLIENTS 1024

class WebServer {
public:
    WebServer(const std::vector<ServerConfig> &configs);
    void run();

private:
    void bindSocket(int &server_fd, int port);
    void handleClient(int client_fd);
    void handleCGI(int client_fd, const RouteConfig &route, const std::string &request_body, const std::string &method, const std::string &query_string, const std::string &content_length, const std::string &content_type, const std::string &url);
    void handleFileUpload(int client_fd, const RouteConfig &route, const std::string &request);

    std::vector<ServerConfig> configs;
    std::vector<int> server_fds;
    std::map<int, int> client_server_map;
    struct pollfd fds[MAX_CLIENTS];
    int nfds;
};

#endif // WEB_SERVER_HPP
