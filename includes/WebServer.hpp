/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   WebServer.hpp                                      :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: ipaslavs <marvin@42.fr>                    +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/10/21 16:33:18 by ipaslavs          #+#    #+#             */
/*   Updated: 2024/10/21 16:33:25 by ipaslavs         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */


#ifndef WEBSERVER_HPP
#define WEBSERVER_HPP

#include <vector>
#include <string>
#include <map>
#include <poll.h>
#include "ServerConfig.hpp"

class WebServer {
public:
    struct ClientConnection {
        int fd;
        std::string request;
        std::string response;
        bool response_ready;
        bool headers_received;
        bool closed;
        size_t response_sent;
        size_t content_length;
        std::string method;
        std::string url;
        std::string http_version;
        std::string content_type;
        std::string query_string;
        std::string transfer_encoding;
        bool chunked_encoding;
        bool chunk_size_received;
        size_t current_chunk_size;
        bool expect_100_continue;
        std::string body;
        const RouteConfig* route;
        std::map<std::string, std::string> headers;

        // CGI fields
        pid_t cgi_pid;
        int cgi_input_fd;
        int cgi_output_fd;
        size_t total_cgi_input_written;
        std::string cgi_output;
        
        // File reading for response
        int file_fd;
        bool reading_file;
        std::string file_path;
        size_t file_bytes_read;
        size_t file_size;

        // File writing for uploads
        int upload_fd;
        bool writing_file;
        size_t file_bytes_written;

        // For CGI requests that might require POST body write
        std::string request_body;
    };

    WebServer(const std::vector<ServerConfig> &configs);
    void run();

private:
    std::vector<ServerConfig> configs;
    std::vector<int> server_fds;
    struct pollfd fds[1024];
    int nfds;
    std::map<int, ClientConnection> clients;
    std::map<int, size_t> client_server_map;

    void setNonBlocking(int socket);
    void bindSocket(int &server_fd, int port);
    void addFdToPoll(int fd, short events);
    void removeFdFromPoll(int fd);
    bool is_closed_socket(const struct pollfd &pfd);

    void handleNewConnection(int server_fd);
    void handleClientRead(int client_fd);
    void handleClientWrite(int client_fd);
    void handleFileRead(int file_fd);
    void handleFileWrite(int file_fd);
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
    void prepareFileResponse(ClientConnection &client, const std::string &file_path);
    void prepareFileUpload(ClientConnection &client, const std::string &upload_path);
    void handleFileUpload(ClientConnection &client);

    void sendErrorResponse(ClientConnection &client, int status_code, const std::string &additional_headers = "");
    std::string getStatusText(int status_code);
    std::string getDefaultErrorPage(int status_code);

    std::string constructFilePath(const RouteConfig &route, const std::string &url);
    std::string generateDirectoryListing(const std::string &path, const std::string &url);
    std::string getMimeType(const std::string &path);

    void handleCGI(int client_fd);
    void processCGIOutput(ClientConnection &client);

    ClientConnection* findClientByCGIFD(int fd);
    ClientConnection* findClientByFileFD(int fd);
    ClientConnection* findClientByUploadFD(int fd);

    std::string intToString(int value);

    // Hard-coded minimal error pages to avoid file I/O for them
    std::string getErrorPageContent(int status_code);
};

#endif
