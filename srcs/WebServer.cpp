/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   WebServer.cpp                                      :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: ipaslavs <marvin@42.fr>                    +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/10/21 16:30:15 by ipaslavs          #+#    #+#             */
/*   Updated: 2024/10/21 16:30:20 by ipaslavs         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */


#include "WebServer.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <signal.h>
#include <poll.h>
#include <cstdlib>

#define BUFFER_SIZE 2048
#define MAX_CLIENTS 1024

WebServer::WebServer(const std::vector<ServerConfig> &configs)
    : configs(configs), nfds(0) {
    signal(SIGPIPE, SIG_IGN);
    server_fds.resize(configs.size());

    for (size_t i = 0; i < configs.size(); ++i) {
        bindSocket(server_fds[i], configs[i].port);
        fds[nfds].fd = server_fds[i];
        fds[nfds].events = POLLIN;
        nfds++;
    }
}

void WebServer::setNonBlocking(int socket) {
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) {
        std::cerr << "fcntl F_GETFL error" << std::endl;
        exit(EXIT_FAILURE);
    }
    if (fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        std::cerr << "fcntl F_SETFL error" << std::endl;
        exit(EXIT_FAILURE);
    }
}

void WebServer::bindSocket(int &server_fd, int port) {
    int opt = 1;
    struct sockaddr_in address;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        std::cerr << "Socket creation error" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt))) {
        std::cerr << "Setsockopt error" << std::endl;
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 128) < 0) {
        std::cerr << "Listen error" << std::endl;
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    setNonBlocking(server_fd);

    std::cout << "Server listening on port " << port << std::endl;
}

bool WebServer::is_closed_socket(const struct pollfd &pfd) {
    return pfd.fd == -1;
}

void WebServer::addFdToPoll(int fd, short events) {
    std::cout << "Adding fd " << fd << " to poll with events " << events << std::endl;
    if (nfds >= MAX_CLIENTS) {
        std::cerr << "Exceeded maximum number of file descriptors" << std::endl;
        return;
    }
    fds[nfds].fd = fd;
    fds[nfds].events = events;
    nfds++;
}

void WebServer::removeFdFromPoll(int fd) {
    for (int i = 0; i < nfds; ++i) {
        if (fds[i].fd == fd) {
            std::cout << "Removing fd " << fd << " from poll array" << std::endl;
            // Replace the current fd with the last fd in the array
            fds[i] = fds[nfds - 1];
            nfds--;
            break;
        }
    }
}

WebServer::ClientConnection* WebServer::findClientByCGIFD(int fd) {
    for (std::map<int, ClientConnection>::iterator it = clients.begin(); it != clients.end(); ++it) {
        if (it->second.cgi_input_fd == fd || it->second.cgi_output_fd == fd) {
            return &it->second;
        }
    }
    return NULL;
}

void WebServer::run() {
    while (true) {
        int poll_count = poll(fds, nfds, -1);

        if (poll_count < 0) {
            std::cerr << "Poll error" << std::endl;
            exit(EXIT_FAILURE);
        }
        
        std::cout << "Poll returned with " << poll_count << " events" << std::endl;

        // Process fds in reverse order
        for (int i = nfds - 1; i >= 0; --i) {
            if (fds[i].revents == 0)
                continue;

            int fd = fds[i].fd;

            if (fds[i].revents & POLLIN) {
                std::cout << "POLLIN event for fd: " << fd << std::endl;
                if (std::find(server_fds.begin(), server_fds.end(), fd) != server_fds.end()) {
                    handleNewConnection(fd);
                } else {
                    if (clients.find(fd) != clients.end()) {
                        handleClientRead(fd);
                    } else {
                        handleCGIRead(fd);
                    }
                }
            }

            if (fds[i].revents & POLLOUT) {
                std::cout << "POLLOUT event for fd: " << fd << std::endl;
                if (clients.find(fd) != clients.end()) {
                    ClientConnection &client = clients[fd];
                    if (client.response_ready) {
                        handleClientWrite(fd);
                    } else {
                        // Remove POLLOUT event if response is not ready
                        fds[i].events &= ~POLLOUT;
                        std::cout << "Removed POLLOUT event for client " << fd << " as response is not ready" << std::endl;
                    }
                } else {
                    handleCGIWrite(fd);
                }
            } 

            if (fds[i].revents & (POLLHUP | POLLERR)) {
                std::cout << "POLLHUP or POLLERR event for fd: " << fd << std::endl;
                if (clients.find(fd) != clients.end()) {
                    handleDisconnection(fd);
                } else {
                    // This might be a CGI fd
                    ClientConnection *client = findClientByCGIFD(fd);
                    if (client) {
                        if (fd == client->cgi_output_fd) {
                            handleCGIRead(fd);
                        } else if (fd == client->cgi_input_fd) {
                            close(client->cgi_input_fd);
                            client->cgi_input_fd = -1;
                            removeFdFromPoll(fd);
                        }
                    } else {
                        close(fd);
                        removeFdFromPoll(fd);
                    }
                }
            }
        }
    }
}

void WebServer::handleNewConnection(int server_fd) {
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    int new_socket;

    while ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) >= 0) {
        std::cout << "New connection on socket " << new_socket << std::endl;

        setNonBlocking(new_socket);

        addFdToPoll(new_socket, POLLIN | POLLOUT | POLLERR | POLLHUP);

        ClientConnection client;
        client.fd = new_socket;
        client.request = "";
        client.response = "";
        client.response_sent = 0;
        client.headers_received = false;
        client.response_ready = false;
        client.closed = false;
        client.content_length = 0;
        client.method = "";
        client.url = "";
        client.http_version = "";
        client.content_type = "";
        client.query_string = "";
        client.transfer_encoding = "";
        client.body = "";
        client.route = NULL;
        client.cgi_pid = -1;
        client.cgi_input_fd = -1;
        client.cgi_output_fd = -1;
        client.total_cgi_input_written = 0;
        client.request_body = "";
        client.cgi_output = "";
        client.chunked_encoding = false;
        client.current_chunk_size = 0;
        client.chunk_size_received = false;
        clients[new_socket] = client;

        client_server_map[new_socket] = std::find(server_fds.begin(), server_fds.end(), server_fd) - server_fds.begin();
    }
}

void WebServer::handleClientRead(int client_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t valread = recv(client_fd, buffer, sizeof(buffer), 0);

    std::cout << "Handling client read for fd: " << client_fd << std::endl;

    if (valread > 0) {
        std::cout << "Read " << valread << " bytes from client" << std::endl;
        ClientConnection &client = clients[client_fd];
        client.request.append(buffer, valread);

        std::cout << "Current request size: " << client.request.size() << " bytes" << std::endl;

        if (!client.headers_received) {
            size_t header_end = client.request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                parseRequestHeaders(client);
                client.request = client.request.substr(header_end + 4);
                
                if (client.expect_100_continue) {
                    std::string continue_response = "HTTP/1.1 100 Continue\r\n\r\n";
                    client.expect_100_continue = false;
                }
            }
        }

        if (client.headers_received)
        {
            if (client.chunked_encoding)
            {
                processChunkedRequest(client);
            }
            else if (client.content_length > 0)
            {
                client.body += client.request;
                client.request.clear(); // Clear after appending
                if (client.body.size() >= client.content_length)
                {
                    client.body = client.body.substr(0, client.content_length);
                    processRequest(client_fd);
                }
            }
            else
            {
                processRequest(client_fd);
            }
        }
    } else if (valread == 0) {
        std::cout << "Client disconnected on fd: " << client_fd << std::endl;
        handleDisconnection(client_fd);
    } else {
        std::cerr << "Error reading from client on fd: " << client_fd << std::endl;
        handleDisconnection(client_fd);
    }
}

void WebServer::handleClientWrite(int client_fd) {
    ClientConnection &client = clients[client_fd];

    std::cout << "Handling client write for fd: " << client_fd << std::endl;

    if (!client.response_ready) {
        std::cout << "Response not ready for client " << client_fd << std::endl;
        return;
    }

    ssize_t to_send = client.response.size() - client.response_sent;
    if (to_send > 0) {
        ssize_t sent = send(client_fd, client.response.c_str() + client.response_sent, to_send, 0);
        if (sent > 0) {
            client.response_sent += sent;
            std::cout << "Sent " << sent << " bytes to client " << client_fd << std::endl;
            if (client.response_sent >= client.response.size()) {
                std::cout << "Response fully sent to client " << client_fd << std::endl;
                handleDisconnection(client_fd);  // Close the connection after sending the response
            }
        } else if (sent == 0) {
            std::cout << "Connection closed while sending to client " << client_fd << std::endl;
            handleDisconnection(client_fd);
        } else {
            std::cerr << "Error sending data to client " << client_fd << ": " << strerror(errno) << std::endl;
            handleDisconnection(client_fd);
        }
    } else {
        std::cout << "No data to send to client " << client_fd << std::endl;
        handleDisconnection(client_fd);  // Close the connection if there's no data to send
    }
}

void WebServer::parseRequestHeaders(ClientConnection &client) {
    std::string request = client.request;
    size_t headers_end = request.find("\r\n\r\n");
    std::string headers = request.substr(0, headers_end);
    std::istringstream request_stream(headers);
    std::string line;

    // Parse the request line
    std::getline(request_stream, line);
    std::istringstream request_line_stream(line);
    request_line_stream >> client.method >> client.url >> client.http_version;

    std::cout << "Parsing request headers:" << std::endl;
    std::cout << "  Method: " << client.method << std::endl;
    std::cout << "  URL: " << client.url << std::endl;
    std::cout << "  HTTP Version: " << client.http_version << std::endl;

    // Parse headers
    while (std::getline(request_stream, line) && line != "\r") {
        size_t pos = line.find(": ");
        if (pos != std::string::npos) {
            std::string header_name = line.substr(0, pos);
            std::string header_value = line.substr(pos + 2);

            // Remove carriage return
            if (!header_value.empty() && header_value[header_value.size() - 1] == '\r') {
                header_value = header_value.substr(0, header_value.size() - 1);
            }

            std::cout << "  Header: " << header_name << " = " << header_value << std::endl;

            if (header_name == "Content-Length") {
                client.content_length = std::atoi(header_value.c_str());
                std::cout << "    Content-Length set to: " << client.content_length << std::endl;
            } else if (header_name == "Content-Type") {
                client.content_type = header_value;
                std::cout << "    Content-Type set to: " << client.content_type << std::endl;
            } else if (header_name == "Transfer-Encoding") {
                client.transfer_encoding = header_value;
                if (header_value == "chunked") {
                    client.chunked_encoding = true;
                    std::cout << "    Chunked encoding detected" << std::endl;
                }
            } else if (header_name == "Expect" && header_value == "100-continue") {
                client.expect_100_continue = true;
                std::cout << "    Expect: 100-continue detected" << std::endl;
            }
        }
    }

    // Parse query string
    size_t pos = client.url.find('?');
    if (pos != std::string::npos) {
        client.query_string = client.url.substr(pos + 1);
        client.url = client.url.substr(0, pos);
        std::cout << "  Query string: " << client.query_string << std::endl;
    }

    client.headers_received = true;
    std::cout << "Headers parsing complete" << std::endl;
}

bool WebServer::isRequestComplete(ClientConnection &client) {
    size_t headers_end = client.request.find("\r\n\r\n");
    if (headers_end == std::string::npos) {
        std::cout << "Headers not yet complete" << std::endl;
        return false;
    }
    
    size_t body_start = headers_end + 4;
    size_t request_size = client.request.size();

    std::cout << "Checking if request is complete:" << std::endl;
    std::cout << "  Headers end at: " << headers_end << std::endl;
    std::cout << "  Body starts at: " << body_start << std::endl;
    std::cout << "  Total request size: " << request_size << std::endl;
    std::cout << "  Expected content length: " << client.content_length << std::endl;

    if (client.chunked_encoding) {
        std::cout << "  Chunked encoding detected, will be processed separately" << std::endl;
        return false;
    } else if (client.content_length > 0) {
        size_t total_size = body_start + client.content_length;
        std::cout << "  Expected total size: " << total_size << std::endl;
        if (request_size >= total_size) {
            client.body = client.request.substr(body_start, client.content_length);
            std::cout << "  Request is complete. Body size: " << client.body.size() << " bytes" << std::endl;
            return true;
        } else {
            std::cout << "  Need more data. Current size: " << request_size << ", Expected: " << total_size << std::endl;
            return false;
        }
    } else {
        std::cout << "  No body expected, request is complete" << std::endl;
        return true;
    }
}

void WebServer::processChunkedRequest(ClientConnection &client) {
    std::string &request = client.request;
    std::string &body = client.body;

    while (!request.empty()) {
        if (!client.chunk_size_received) {
            size_t pos = request.find("\r\n");
            if (pos == std::string::npos) {
                return; // Incomplete chunk size
            }
            std::string chunk_size_str = request.substr(0, pos);
            client.current_chunk_size = strtoul(chunk_size_str.c_str(), NULL, 16);
            client.chunk_size_received = true;
            request.erase(0, pos + 2);
        }

        if (client.current_chunk_size == 0) {
            // End of chunked data
            client.chunked_encoding = false;
            processRequest(client.fd);
            return;
        }

        if (request.size() < client.current_chunk_size + 2) {
            return; // Incomplete chunk
        }

        body.append(request.substr(0, client.current_chunk_size));
        request.erase(0, client.current_chunk_size + 2); // +2 for \r\n
        client.chunk_size_received = false;
    }
}

void WebServer::processRequest(int client_fd) {
    ClientConnection &client = clients[client_fd];
    const ServerConfig &server_config = configs[client_server_map[client_fd]];
    std::string method = client.method;
    std::string url = client.url;
    std::string http_version = client.http_version;

    std::cout << "Processing request for client fd: " << client_fd << std::endl;
    std::cout << "  Method: " << method << std::endl;
    std::cout << "  URL: " << url << std::endl;
    std::cout << "  HTTP Version: " << http_version << std::endl;
    std::cout << "  Content-Type: " << client.content_type << std::endl;
    std::cout << "  Content-Length: " << client.content_length << std::endl;

    // Wait until we have received all the data for POST/PUT requests
    if (client.content_length > 0) {
        if (client.body.size() < client.content_length) {
            std::cout << "Waiting for more data. Current size: " << client.body.size() 
                      << " Expected: " << client.content_length << std::endl;
            return; // Wait for more data
        }
    }

    // Find the best matching route
    const RouteConfig *best_match_route = NULL;
    size_t longest_match_length = 0;

    std::cout << "Searching for matching route..." << std::endl;
    // First Pass: Prefix-Based Matching
    for (size_t i = 0; i < server_config.routes.size(); ++i) {
        const RouteConfig &route = server_config.routes[i];
        std::cout << "  Checking route: " << route.url << std::endl;
        if ((route.url == "/" && url.find(route.url) == 0) ||
            (route.url != "/" && (url == route.url || url.find(route.url + "/") == 0))) {
            if (route.url.size() >= longest_match_length) {
                best_match_route = &route;
                longest_match_length = route.url.size();
                std::cout << "    Found matching route: " << route.url << std::endl;
            }
        }
    }

    // Second Pass: Suffix-Based Matching (Extension-Based)
    for (size_t i = 0; i < server_config.routes.size(); ++i) {
        const RouteConfig &route = server_config.routes[i];
        // Treat routes starting with '.' as extension-based routes
        if (!route.url.empty() && route.url[0] == '.') {
            size_t ext_length = route.url.size();
            if (url.size() >= ext_length &&
                url.compare(url.size() - ext_length, ext_length, route.url) == 0) {

                best_match_route = &route;
                std::cout << "    Found extension-based matching route: " << route.url << std::endl;

            }
        }
    }

    if (!best_match_route) {
        std::cerr << "Route not found for URL: " << url << std::endl;
        sendErrorResponse(client, 404);
        return;
    }

    std::cout << "Best matching route: " << best_match_route->url << std::endl;
    client.route = best_match_route;
    const RouteConfig &route = *best_match_route;

    // Check if the method is allowed
    std::cout << "Checking if method is allowed..." << std::endl;
    if (std::find(route.methods.begin(), route.methods.end(), method) == route.methods.end()) {
        std::string allowed_methods;
        for (size_t i = 0; i < route.methods.size(); ++i) {
            allowed_methods += route.methods[i];
            if (i < route.methods.size() - 1) {
                allowed_methods += ", ";
            }
        }
        std::cout << "Method not allowed. Allowed methods: " << allowed_methods << std::endl;
        sendErrorResponse(client, 405, "Allow: " + allowed_methods);
        return;
    }

    // Handle different HTTP methods
    std::cout << "Handling " << method << " request" << std::endl;
    if (method == "GET") {
        handleGetRequest(client);
    } else if (method == "POST") {
        std::cout << "Received POST request for URL: " << url << std::endl;
        std::cout << "Content-Type: " << client.content_type << std::endl;
        std::cout << "Content-Length: " << client.content_length << std::endl;
        std::cout << "Transfer-Encoding: " << client.transfer_encoding << std::endl;

        if (client.transfer_encoding == "chunked") {
            client.chunked_encoding = true;
        }
        
        // Check max body size
        if (route.max_body > 0 && client.body.size() > route.max_body) {
            std::cout << "Request body exceeds max allowed size" << std::endl;
            sendErrorResponse(client, 413);
            return;
        }

        if (!route.cgi_path.empty()) {
            std::cout << "Handling CGI request" << std::endl;
            handleCGI(client_fd);
        } else if (!route.upload_dir.empty()) {
            std::cout << "Handling file upload to directory: " << route.upload_dir << std::endl;
            handleFileUpload(client);
        } else {
            std::cout << "Handling regular POST request" << std::endl;
            handlePostRequest(client);
        }
    } else if (method == "DELETE") {
        handleDeleteRequest(client);
    } else if (method == "PUT") {
        handlePutRequest(client);
    } else {
        // If the method is not supported
        std::cout << "Unsupported method: " << method << std::endl;
        sendErrorResponse(client, 501); //501
    }

    // After processing, reset the client state
    client.headers_received = false;
    client.body.clear();
    client.request.clear();
    client.content_length = 0;
    client.method.clear();
    client.url.clear();
    client.http_version.clear();
    client.content_type.clear();
    client.query_string.clear();
    client.transfer_encoding.clear();
    client.chunked_encoding = false;
    client.current_chunk_size = 0;
    client.chunk_size_received = false;
}

void WebServer::handleGetRequest(ClientConnection &client) {
    const RouteConfig &route = *client.route;

    std::cout << "Handling GET request for client fd: " << client.fd << std::endl;

    if (!route.cgi_path.empty()) {
        std::cout << "CGI path found, handling CGI request" << std::endl;
        handleCGI(client.fd);
        return;
    }

    std::string file_path = constructFilePath(route, client.url);
    std::cout << "Constructed file path: " << file_path << std::endl;

    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) == -1) {
        std::cout << "File not found: " << file_path << std::endl;
        sendErrorResponse(client, 404);
        return;
    }

    if (S_ISDIR(file_stat.st_mode)) {
        std::cout << "Path is a directory, handling directory request" << std::endl;
        handleDirectoryRequest(client, file_path);
    } else {
        std::cout << "Path is a file, sending file response" << std::endl;
        sendFileResponse(client, file_path);
    }
}

void WebServer::handlePostRequest(ClientConnection &client) {
    const RouteConfig &route = *client.route;

    if (client.content_type.find("multipart/form-data") != std::string::npos) {
        handleFileUpload(client);
    } else if (!route.cgi_path.empty()) {
        handleCGI(client.fd);
    } else {
        // Handle other POST requests
        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.response = response;
        client.response_ready = true;
    }
}

void WebServer::handleDeleteRequest(ClientConnection &client) {
    const RouteConfig &route = *client.route;
    std::string file_path = constructFilePath(route, client.url);

    if (remove(file_path.c_str()) == 0) {
        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.response = response;
    } else {
        sendErrorResponse(client, 404);
    }
    client.response_ready = true;
}

void WebServer::handlePutRequest(ClientConnection &client) {
    const RouteConfig &route = *client.route;
    std::string upload_path = route.upload_dir + client.url.substr(route.url.size());

    // Create directories if they do not exist
    size_t last_slash_pos = upload_path.find_last_of('/');
    if (last_slash_pos != std::string::npos) {
        std::string dir_path = upload_path.substr(0, last_slash_pos);
        if (dir_path.find("..") != std::string::npos) {
            sendErrorResponse(client, 400);
            return;
        }
        mkdir(dir_path.c_str(), 0755);
    }

    std::ofstream outfile(upload_path.c_str(), std::ios::binary);
    if (!outfile.is_open()) {
        sendErrorResponse(client, 500);
        return;
    }

    outfile << client.body;
    outfile.close();

    std::string response = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    client.response = response;
    client.response_ready = true;
}

void WebServer::handleDirectoryRequest(ClientConnection &client, const std::string &dir_path) {
    const RouteConfig &route = *client.route;
    std::string index = route.index;

    if (route.index.empty()) { index = "index.html";}

    if (!index.empty()) {
        std::string index_path = dir_path + "/" + index;
        struct stat index_stat;
        if (stat(index_path.c_str(), &index_stat) != -1) {
            sendFileResponse(client, index_path);
            return;
        } else {
            if (!route.autoindex) {
                sendErrorResponse(client,404);
                return;
            }
        }
    }

    if (route.autoindex) {
        std::string listing = generateDirectoryListing(dir_path, client.url);
        std::ostringstream response_stream;
        response_stream << "HTTP/1.1 200 OK\r\n";
        response_stream << "Content-Type: text/html\r\n";
        response_stream << "Content-Length: " << listing.size() << "\r\n";
        response_stream << "\r\n";
        response_stream << listing;
        client.response = response_stream.str();
        client.response_ready = true;
    } else {
        sendErrorResponse(client, 403);
    }
}

void WebServer::sendFileResponse(ClientConnection &client, const std::string &file_path) {
    std::cout << "Sending file response for: " << file_path << std::endl;

    std::ifstream file(file_path.c_str(), std::ios::binary);
    if (!file.is_open()) {
        std::cout << "Failed to open file: " << file_path << std::endl;
        sendErrorResponse(client, 403);
        return;
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    std::string file_content = oss.str();
    file.close();

    std::string content_type = getMimeType(file_path);

    std::ostringstream response_stream;
    response_stream << "HTTP/1.1 200 OK\r\n";
    response_stream << "Content-Type: " << content_type << "\r\n";
    response_stream << "Content-Length: " << file_content.size() << "\r\n";
    response_stream << "\r\n";
    response_stream << file_content;

    client.response = response_stream.str();
    client.response_ready = true;

    std::cout << "File response prepared. Size: " << client.response.size() << " bytes" << std::endl;

    // Add POLLOUT event for the client's fd
    for (int i = 0; i < nfds; ++i) {
        if (fds[i].fd == client.fd) {
            fds[i].events |= POLLOUT;
            std::cout << "Added POLLOUT event for client fd: " << client.fd << std::endl;
            break;
        }
    }
}

void WebServer::sendErrorResponse(ClientConnection &client, int status_code, const std::string &additional_headers) {
    const ServerConfig &server_config = configs[client_server_map[client.fd]];
    std::string error_body = getErrorPage(server_config, status_code);
    if (error_body.empty()) {
        error_body = getDefaultErrorPage(status_code);
    }

    std::ostringstream response_stream;
    response_stream << "HTTP/1.1 " << status_code << " " << getStatusText(status_code) << "\r\n";
    response_stream << "Content-Type: text/html\r\n";
    response_stream << "Content-Length: " << error_body.size() << "\r\n";
    if (!additional_headers.empty()) {
        response_stream << additional_headers;
    }
    response_stream << "Connection: close\r\n\r\n";
    response_stream << error_body;

    client.response = response_stream.str();
    client.response_ready = true;

    // Add POLLOUT event for the client's fd
    for (int i = 0; i < nfds; ++i) {
        if (fds[i].fd == client.fd) {
            fds[i].events |= POLLOUT;
            std::cout << "Added POLLOUT event for client fd: " << client.fd << std::endl;
            break;
        }
    }
}

void WebServer::handleCGI(int client_fd) {
    ClientConnection &client = clients[client_fd];
    const RouteConfig &route = *client.route;

    std::cout << "Handling CGI for client_fd: " << client_fd << std::endl;

    int cgi_output[2];
    int cgi_input[2];

    if (pipe(cgi_output) < 0 || pipe(cgi_input) < 0) {
        std::cerr << "Failed to create pipes for CGI" << std::endl;
        sendErrorResponse(client, 500);
        return;
    }

    std::cout << "Pipes created successfully" << std::endl;

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork for CGI" << std::endl;
        sendErrorResponse(client, 500);
        return;
    }

    if (pid == 0) {
        // Child process
        std::cout << "CGI child process started" << std::endl;

        close(cgi_output[0]); // Close read end
        close(cgi_input[1]);  // Close write end

        dup2(cgi_output[1], STDOUT_FILENO); // Redirect stdout to cgi_output[1]
        dup2(cgi_input[0], STDIN_FILENO);   // Redirect stdin to cgi_input[0]

        close(cgi_output[1]);
        close(cgi_input[0]);

        // Set up environment variables
        extern char **environ;
        
        // Extract the script name from the URL
        std::string script_name = client.url.substr(client.url.find_last_of('/') + 1);

        setenv("REQUEST_METHOD", client.method.c_str(), 1);
        setenv("QUERY_STRING", client.query_string.c_str(), 1);
        setenv("CONTENT_LENGTH", intToString(client.body.size()).c_str(), 1);
        setenv("CONTENT_TYPE", client.content_type.c_str(), 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);

        // Corrected SCRIPT_NAME and PATH_INFO
 //       setenv("SCRIPT_NAME", client.url.c_str(), 1);
 //       setenv("PATH_INFO", client.url.c_str(), 1);
  //      setenv("PATH_INFO", "youpi.bla", 1);
        setenv("PATH_INFO", script_name.c_str(), 1);

        // Set PATH_TRANSLATED to map PATH_INFO to a physical path
        std::string path_translated = route.root + client.url;
        setenv("PATH_TRANSLATED", path_translated.c_str(), 1);

        // Additional environment variables
        setenv("SERVER_NAME", "localhost", 1);
        setenv("SERVER_PORT", intToString(configs[client_server_map[client.fd]].port).c_str(), 1);
        setenv("REMOTE_ADDR", "127.0.0.1", 1);
        setenv("REDIRECT_STATUS", "200", 1); // Some scripts expect this to be set

        setenv("HTTP_X_SECRET_HEADER_FOR_TEST", "1", 1);

        // Change the working directory if necessary
        std::string script_dir = route.cgi_path.substr(0, route.cgi_path.find_last_of("/"));
        if (!script_dir.empty() && chdir(script_dir.c_str()) != 0) {
            std::cerr << "Failed to change directory to: " << script_dir << std::endl;
        }

        // Execute the CGI script
        char *args[] = {const_cast<char *>(route.cgi_path.c_str()), NULL};
        execve(route.cgi_path.c_str(), args, environ);

        // If execve fails
        std::cerr << "Failed to execve CGI script: " << route.cgi_path << std::endl;
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        std::cout << "CGI parent process continuing" << std::endl;
        close(cgi_output[1]); // Close write end
        close(cgi_input[0]);  // Close read end

        fcntl(cgi_output[0], F_SETFL, O_NONBLOCK);
        fcntl(cgi_input[1], F_SETFL, O_NONBLOCK);

        // Store CGI state in client connection
        client.cgi_pid = pid;
        client.cgi_input_fd = cgi_input[1];
        client.cgi_output_fd = cgi_output[0];
        client.total_cgi_input_written = 0;
        client.cgi_output = "";

        // Add the CGI fds to the pollfd array
        addFdToPoll(client.cgi_input_fd, POLLOUT);
        addFdToPoll(client.cgi_output_fd, POLLIN);

        std::cout << "CGI setup complete. Input fd: " << client.cgi_input_fd << ", Output fd: " << client.cgi_output_fd << std::endl;
    }
}

void WebServer::setupCGIEnvironment(ClientConnection &client) {
    const RouteConfig &route = *client.route;

    setenv("REQUEST_METHOD", client.method.c_str(), 1);
    setenv("QUERY_STRING", client.query_string.c_str(), 1);
    setenv("CONTENT_LENGTH", intToString(client.body.size()).c_str(), 1);
    setenv("CONTENT_TYPE", client.content_type.c_str(), 1);
    setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
    setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
    setenv("REMOTE_ADDR", "", 1);
    setenv("PATH_INFO", client.url.c_str(), 1);
    setenv("PATH_TRANSLATED", (route.root + client.url).c_str(), 1);
    setenv("SCRIPT_NAME", route.cgi_path.c_str(), 1);
    setenv("DOCUMENT_ROOT", route.root.c_str(), 1);
    setenv("SERVER_NAME", "webserv", 1);
    setenv("SERVER_PORT", intToString(configs[client_server_map[client.fd]].port).c_str(), 1);
    setenv("REDIRECT_STATUS", "200", 1);

    // Change the working directory to the script's directory
    std::string script_dir = route.cgi_path.substr(0, route.cgi_path.find_last_of("/"));
    if (chdir(script_dir.c_str()) != 0) {
        std::cerr << "Failed to change directory to: " << script_dir << std::endl;
    }
}

void WebServer::handleCGIWrite(int cgi_input_fd) 
{
    ClientConnection *client = findClientByCGIFD(cgi_input_fd);
    if (!client) 
    {
        close(cgi_input_fd);
        removeFdFromPoll(cgi_input_fd);
        return;
    }

    size_t remaining = client->body.size() - client->total_cgi_input_written;
    if (remaining > 0) 
    {
        ssize_t written = write(cgi_input_fd, 
                              client->body.c_str() + client->total_cgi_input_written, 
                              remaining);

        if (written > 0) 
        {
            client->total_cgi_input_written += written;
            
            // Check if all data has been written
            if (client->total_cgi_input_written == client->body.size()) 
            {
                close(cgi_input_fd);
                client->cgi_input_fd = -1;
                removeFdFromPoll(cgi_input_fd);
            }
        }
        else 
        {
            // Handle both error (written < 0) and connection closed (written == 0)
            std::cout << "Error writing to CGI input fd: " << cgi_input_fd << std::endl;
            
            // Clean up CGI input
            close(cgi_input_fd);
            client->cgi_input_fd = -1;
            removeFdFromPoll(cgi_input_fd);
            
            // Remove client
            handleDisconnection(client->fd);
        }
    }
    else 
    {
        // All data has been written
        close(cgi_input_fd);
        client->cgi_input_fd = -1;
        removeFdFromPoll(cgi_input_fd);
    }
}

void WebServer::handleCGIRead(int cgi_output_fd) {
    ClientConnection *client = findClientByCGIFD(cgi_output_fd);
    if (!client) {
        std::cerr << "No client found for CGI output fd: " << cgi_output_fd << std::endl;
        // Close and remove the fd since it's no longer associated with a client
        close(cgi_output_fd);
        removeFdFromPoll(cgi_output_fd);
        return;
    }

    std::cout << "Handling CGI read for fd: " << cgi_output_fd << std::endl;

    char buffer[BUFFER_SIZE];
    ssize_t nbytes = read(cgi_output_fd, buffer, sizeof(buffer));

    if (nbytes > 0) {
        std::cout << "Read " << nbytes << " bytes from CGI output" << std::endl;
        client->cgi_output.append(buffer, nbytes);
    } else if (nbytes == 0) {
        // EOF reached
        std::cout << "CGI output ended for fd: " << cgi_output_fd << std::endl;

        // Close and clean up the CGI output file descriptor
        close(cgi_output_fd);
        client->cgi_output_fd = -1;
        removeFdFromPoll(cgi_output_fd);

        // Wait for the CGI process to exit and clean up
        if (client->cgi_pid != -1) {
            int status;
            waitpid(client->cgi_pid, &status, 0);
            std::cout << "CGI process exited with status: " << status << std::endl;
            client->cgi_pid = -1;
        }

        // Process CGI output and prepare the response
        processCGIOutput(*client);

        // Mark the response as ready to be sent
        client->response_ready = true;

        // Add POLLOUT event for the client's fd to send the response
        for (int j = 0; j < nfds; ++j) {
            if (fds[j].fd == client->fd) {
                fds[j].events |= POLLOUT;
                std::cout << "Added POLLOUT event for client fd: " << client->fd << std::endl;
                break;
            }
        }

        std::cout << "CGI response ready to be sent for client fd: " << client->fd << std::endl;
    } else {
        // Error occurred
        std::cerr << "Error reading from CGI output fd: " << cgi_output_fd << " - " << strerror(errno) << std::endl;

        // Close and clean up
        close(cgi_output_fd);
        client->cgi_output_fd = -1;
        removeFdFromPoll(cgi_output_fd);

        // Send error response to the client
        sendErrorResponse(*client, 500);

        // Mark the response as ready to be sent
        client->response_ready = true;

        // Add POLLOUT event for the client's fd to send the response
        for (int j = 0; j < nfds; ++j) {
            if (fds[j].fd == client->fd) {
                fds[j].events |= POLLOUT;
                std::cout << "Added POLLOUT event for client fd: " << client->fd << std::endl;
                break;
            }
        }
    }
}

void WebServer::processCGIOutput(ClientConnection &client) {
    std::cout << "Processing CGI output for client fd: " << client.fd << std::endl;
    std::cout << "CGI output size: " << client.cgi_output.size() << " bytes" << std::endl;

    if (client.cgi_output.empty()) {
        std::cout << "CGI output is empty, sending error response" << std::endl;
        sendErrorResponse(client, 500);
        return;
    }

    // Find the end of headers (\r\n\r\n)
    size_t header_end = client.cgi_output.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        std::cout << "Malformed CGI output: Headers not terminated correctly" << std::endl;
        sendErrorResponse(client, 500);
        return;
    }

    // Extract headers and body
    std::string headers = client.cgi_output.substr(0, header_end);
    std::string body = client.cgi_output.substr(header_end + 4); // Skip "\r\n\r\n"

    std::cout << "CGI Headers:\n" << headers << std::endl;
    std::cout << "CGI Body (first 100 bytes):\n" << body.substr(0, 100) << std::endl;

    // Check if Content-Type is already present
    bool has_content_type = false;
    std::istringstream header_stream(headers);
    std::string line;
    while (std::getline(header_stream, line)) {
        // Remove any trailing '\r'
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }

        if (line.find("Content-Type:") != std::string::npos) {
            has_content_type = true;
            break;
        }
    }

    std::ostringstream response_stream;
    response_stream << "HTTP/1.1 200 OK\r\n";

    if (!has_content_type) {
        response_stream << "Content-Type: text/html\r\n";
    }

    // Append CGI headers
    response_stream << headers << "\r\n"; // Ensure headers end with CRLF

    // Append Content-Length
    response_stream << "Content-Length: " << body.size() << "\r\n";
    response_stream << "\r\n"; // End of headers

    // Append body
    response_stream << body;

    client.response = response_stream.str();
    client.response_ready = true;

    std::cout << "CGI response prepared. Size: " << client.response.size() << " bytes" << std::endl;
    std::cout << "CGI response headers:\n" << headers << std::endl;
    std::cout << "CGI response body (first 100 bytes):\n" << body.substr(0, 100) << std::endl;
}


void WebServer::handleFileUpload(ClientConnection &client) {
    std::cout << "Handling file upload for client fd: " << client.fd << std::endl;
    std::cout << "Request body size: " << client.body.size() << " bytes" << std::endl;
    std::cout << "Content-Type: " << client.content_type << std::endl;

    const RouteConfig &route = *client.route;

    if (client.body.empty()) {
        std::cout << "Empty request body, sending 400 Bad Request" << std::endl;
        sendErrorResponse(client, 400);
        return;
    }

    std::string filename;
    std::string file_content;

    if (client.chunked_encoding || client.content_type == "application/x-www-form-urlencoded") {
        // For chunked uploads or url-encoded data, generate a filename
        std::ostringstream filename_stream;
        filename_stream << "upload_" << time(NULL);
        
        // Try to add an appropriate extension based on the Content-Type
        if (client.content_type == "application/json") {
            filename_stream << ".json";
        } else if (client.content_type == "text/plain") {
            filename_stream << ".txt";
        } else if (client.content_type == "application/xml") {
            filename_stream << ".xml";
        }
        // Add more content types as needed
        
        filename = filename_stream.str();
        file_content = client.body;
    } else if (client.content_type.find("multipart/form-data") != std::string::npos) {
        // For multipart form-data, extract filename and content
        std::string boundary_prefix = "boundary=";
        size_t boundary_pos = client.content_type.find(boundary_prefix);
        if (boundary_pos == std::string::npos) {
            std::cerr << "Boundary not found in Content-Type header" << std::endl;
            sendErrorResponse(client, 400);
            return;
        }
        std::string boundary = "--" + client.content_type.substr(boundary_pos + boundary_prefix.length());

        size_t pos = client.body.find("filename=\"");
        if (pos != std::string::npos) {
            size_t filename_start = pos + 10;
            size_t filename_end = client.body.find("\"", filename_start);
            filename = client.body.substr(filename_start, filename_end - filename_start);
        } else {
            filename = "uploaded_file_" + intToString(time(NULL));
        }

        size_t content_start = client.body.find("\r\n\r\n");
        if (content_start != std::string::npos) {
            content_start += 4;
            size_t content_end = client.body.rfind(boundary) - 2;
            if (content_end > content_start) {
                file_content = client.body.substr(content_start, content_end - content_start);
            }
        }
    } else {
        std::cerr << "Unsupported Content-Type for file upload" << std::endl;
        sendErrorResponse(client, 400);
        return;
    }

    std::string filepath = route.upload_dir + "/" + filename;
    std::ofstream outfile(filepath.c_str(), std::ios::binary);
    if (!outfile.is_open()) {
        std::cerr << "Failed to open file for writing: " << filepath << std::endl;
        sendErrorResponse(client, 500);
        return;
    }
    
    outfile.write(file_content.c_str(), file_content.size());
    outfile.close();

    std::cout << "File saved: " << filepath << std::endl;

    // Send response
    std::string response_body = "<html><body><h1>File Uploaded Successfully</h1><p>Saved as: " + filename + "</p></body></html>";
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n";
    oss << "Content-Type: text/html\r\n";
    oss << "Content-Length: " << response_body.size() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << response_body;
    client.response = oss.str();
    client.response_ready = true;

    std::cout << "File upload response prepared. Size: " << client.response.size() << " bytes" << std::endl;

    // Add POLLOUT event for the client's fd
    for (int i = 0; i < nfds; ++i) {
        if (fds[i].fd == client.fd) {
            fds[i].events |= POLLOUT;
            std::cout << "Added POLLOUT event for client fd: " << client.fd << std::endl;
            break;
        }
    }
}

std::string WebServer::constructFilePath(const RouteConfig &route, const std::string &url) {
    std::string file_path;
    if (!route.alias.empty()) {
        file_path = route.alias;
        std::string remaining_url = url.substr(route.url.size());
        if (!remaining_url.empty() && remaining_url[0] == '/') {
            remaining_url = remaining_url.substr(1);
        }
        if (!file_path.empty() && file_path[file_path.size() - 1] != '/' && !remaining_url.empty()) {
            file_path += "/";
        }
        file_path += remaining_url;
    } else {
        file_path = route.root;
        std::string remaining_url = url.substr(route.url.size());
        if (!remaining_url.empty() && remaining_url[0] == '/') {
            remaining_url = remaining_url.substr(1);
        }
        if (!file_path.empty() && file_path[file_path.size() - 1] != '/' && !remaining_url.empty()) {
            file_path += "/";
        }
        file_path += remaining_url;
    }
    return file_path;
}

std::string WebServer::generateDirectoryListing(const std::string &path, const std::string &url) {
    std::stringstream listing;
    listing << "<html><head><title>Directory listing for " << url << "</title></head><body>";
    listing << "<h1>Directory listing for " << url << "</h1>";
    listing << "<ul>";

    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(path.c_str())) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            std::string entry_name = ent->d_name;
            std::string entry_url = url;
            if (entry_url[entry_url.size() - 1] != '/') {
                entry_url += "/";
            }
            entry_url += entry_name;
            listing << "<li><a href=\"" << entry_url << "\">" << entry_name << "</a></li>";
        }
        closedir(dir);
    } else {
        std::cerr << "Failed to open directory: " << path << std::endl;
        return "";
    }

    listing << "</ul></body></html>";
    return listing.str();
}

std::string WebServer::getMimeType(const std::string &path) {
    std::string extension = path.substr(path.find_last_of(".") + 1);
    if (extension == "html" || extension == "htm") return "text/html";
    if (extension == "css") return "text/css";
    if (extension == "js") return "application/javascript";
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "png") return "image/png";
    if (extension == "gif") return "image/gif";
    return "application/octet-stream";
}

std::string WebServer::getErrorPage(const ServerConfig &config, int status_code) {
    std::map<int, std::string>::const_iterator it = config.error_pages.find(status_code);
    if (it != config.error_pages.end()) {
        std::ifstream error_file(it->second.c_str());
        if (error_file.is_open()) {
            std::stringstream buffer;
            buffer << error_file.rdbuf();
            return buffer.str();
        }
    }
    return "";
}

std::string WebServer::getDefaultErrorPage(int status_code) {
    std::ostringstream oss;
    oss << "<!DOCTYPE html><html><head><title>" << status_code << " " << getStatusText(status_code) << "</title></head>"
        << "<body><h1>" << status_code << " " << getStatusText(status_code) << "</h1></body></html>";
    return oss.str();
}

std::string WebServer::getStatusText(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default: return "Unknown Status";
    }
}

std::string WebServer::intToString(int value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

void WebServer::handleDisconnection(int fd) {
    std::cout << "Handling disconnection for fd: " << fd << std::endl;
    
    std::map<int, ClientConnection>::iterator it = clients.find(fd);
    if (it != clients.end()) {
        ClientConnection &client = it->second;
        std::cout << "Disconnecting client on socket " << fd << std::endl;
        
        // Close client fd
        close(fd);
        removeFdFromPoll(fd);
        
        // Close CGI input fd if open
        if (client.cgi_input_fd != -1) {
            close(client.cgi_input_fd);
            removeFdFromPoll(client.cgi_input_fd);
            client.cgi_input_fd = -1;
        }
        
        // Close CGI output fd if open
        if (client.cgi_output_fd != -1) {
            close(client.cgi_output_fd);
            removeFdFromPoll(client.cgi_output_fd);
            client.cgi_output_fd = -1;
        }
        
        // Terminate CGI process if running
        if (client.cgi_pid != -1) {
            kill(client.cgi_pid, SIGKILL);
            waitpid(client.cgi_pid, NULL, 0); // Wait for the process to terminate
            client.cgi_pid = -1;
        }
        
        clients.erase(it);
    } else {
        std::cout << "Closing non-client fd: " << fd << std::endl;
        close(fd);
        removeFdFromPoll(fd);
    }
}
