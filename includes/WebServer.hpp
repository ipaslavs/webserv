// WebServer.hpp
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
        pid_t cgi_pid;
        int cgi_input_fd;
        int cgi_output_fd;
        size_t total_cgi_input_written;
        std::string request_body;
        std::string cgi_output;
        bool chunked_encoding;
        size_t current_chunk_size;
        bool chunk_size_received;
        bool expect_100_continue;
    };
    std::map<int, ClientConnection> clients;

    void bindSocket(int &server_fd, int port);
    void setNonBlocking(int socket);
    static bool is_closed_socket(const struct pollfd &pfd);
    void addFdToPoll(int fd, short events);
    void removeFdFromPoll(int fd);
    ClientConnection* findClientByCGIFD(int fd);

    void handleNewConnection(int server_fd);
    void handleClientRead(int client_fd);
    void handleClientWrite(int client_fd);
    void handleCGIRead(int cgi_output_fd);
    void handleCGIWrite(int cgi_input_fd);
    void handleDisconnection(int fd);

    void parseRequestHeaders(ClientConnection &client);
    bool isRequestComplete(ClientConnection &client);
    void processChunkedRequest(ClientConnection &client);
    void processRequest(int client_fd);

    void handleGetRequest(ClientConnection &client);
    void handlePostRequest(ClientConnection &client);
    void handleDeleteRequest(ClientConnection &client);
    void handlePutRequest(ClientConnection &client);
    void handleDirectoryRequest(ClientConnection &client, const std::string &dir_path);
    void handleFileUpload(ClientConnection &client);

    void handleCGI(int client_fd);
    void setupCGIEnvironment(ClientConnection &client);
    void processCGIOutput(ClientConnection &client);

    void sendFileResponse(ClientConnection &client, const std::string &file_path);
    void sendErrorResponse(ClientConnection &client, int status_code, const std::string &additional_headers = "");

    static std::string constructFilePath(const RouteConfig &route, const std::string &url);
    static std::string generateDirectoryListing(const std::string &path, const std::string &url);
    static std::string getMimeType(const std::string &path);
    static std::string getErrorPage(const ServerConfig &config, int status_code);
    static std::string getDefaultErrorPage(int status_code);
    static std::string getStatusText(int status_code);
    static std::string intToString(int value);
};

#endif // WEBSERVER_HPP