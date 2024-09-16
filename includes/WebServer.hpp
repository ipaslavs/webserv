#ifndef WEBSERVER_HPP
#define WEBSERVER_HPP

#include "ServerConfig.hpp"
#include <vector>
#include <map>
#include <string>
#include <poll.h>

class WebServer {
public:
    WebServer(const std::vector<ServerConfig> &configs);
    void run();
private:
    std::vector<ServerConfig> configs;
    std::vector<int> server_fds;
    struct pollfd fds[1024];
    int nfds;
    std::map<int, int> client_server_map;

    void bindSocket(int &server_fd, int port);
    void handleClientRead(int fd);
    void handleClientWrite(int fd);
    void processRequest(int client_fd);
    void setNonBlocking(int socket);
    static bool is_closed_socket(const struct pollfd &pfd);

    struct ClientConnection {
        int fd;
        std::string request;
        std::string response;
        size_t response_sent;
        bool headers_received;
        bool response_ready;
        bool closed;
        size_t content_length;
        std::string method;
        std::string url;
        std::string http_version;
        std::string content_type;
        std::string query_string;
        std::string transfer_encoding;
        std::string body;
        const RouteConfig *route;
        // Additional state for CGI handling
        pid_t cgi_pid;
        int cgi_input_fd;
        int cgi_output_fd;
        size_t total_cgi_input_written;
        std::string request_body;
        std::string cgi_output;
        // For DELETE method
        std::string file_path;
    };
    std::map<int, ClientConnection> clients;

    // Helper functions
    static std::string intToString(int value);
    static std::string getMimeType(const std::string &path);
    static std::string getErrorPage(const ServerConfig &config, int status_code);
    static std::string getStatusText(int status_code);
    static std::string getDefaultErrorPage(int status_code);
    static std::string generateDirectoryListing(const std::string &path, const std::string &url);

    void handleCGI(int client_fd);
    void handleCGIWrite(int cgi_input_fd);
    void handleCGIRead(int cgi_output_fd);
    void handleFileUpload(int client_fd);
    void handleDeleteRequest(int client_fd);

    // Additional helper functions
    void parseRequestHeaders(ClientConnection &client);
    bool isRequestComplete(ClientConnection &client);
    ClientConnection* findClientByCGIFD(int fd);
    void addFdToPoll(int fd, short events);
    void removeFdFromPoll(int fd);
};

#endif // WEBSERVER_HPP
