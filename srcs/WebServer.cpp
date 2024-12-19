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
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <signal.h>
#include <poll.h>
#include <cstdlib>
#include <cstdio>

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

WebServer::ClientConnection* WebServer::findClientByFileFD(int fd) {
    for (std::map<int, ClientConnection>::iterator it = clients.begin(); it != clients.end(); ++it) {
        if (it->second.file_fd == fd && it->second.reading_file) {
            return &it->second;
        }
    }
    return NULL;
}

WebServer::ClientConnection* WebServer::findClientByUploadFD(int fd) {
    for (std::map<int, ClientConnection>::iterator it = clients.begin(); it != clients.end(); ++it) {
        if (it->second.upload_fd == fd && it->second.writing_file) {
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

        for (int i = nfds - 1; i >= 0; --i) {
            if (fds[i].revents == 0)
                continue;

            int fd = fds[i].fd;

            if (fds[i].revents & POLLIN) {
                // Identify what fd is this: server socket, client socket, cgi fd, file fd, upload fd
                if (std::find(server_fds.begin(), server_fds.end(), fd) != server_fds.end()) {
                    handleNewConnection(fd);
                } else if (clients.find(fd) != clients.end()) {
                    handleClientRead(fd);
                } else if (findClientByCGIFD(fd) != NULL) {
                    handleCGIRead(fd);
                } else if (findClientByFileFD(fd) != NULL) {
                    handleFileRead(fd);
                } else {
                    // Could be unexpected, just close it
                    close(fd);
                    removeFdFromPoll(fd);
                }
            }

            if (fds[i].revents & POLLOUT) {
                // check if it's a client fd or cgi fd or file upload fd
                ClientConnection* client_cgi = findClientByCGIFD(fd);
                ClientConnection* client_upload = findClientByUploadFD(fd);
                if (clients.find(fd) != clients.end()) {
                    ClientConnection &client = clients[fd];
                    if (client.response_ready) {
                        handleClientWrite(fd);
                    } else {
                        fds[i].events &= ~POLLOUT;
                    }
                } else if (client_cgi != NULL) {
                    handleCGIWrite(fd);
                } else if (client_upload != NULL) {
                    handleFileWrite(fd);
                } else {
                    // Unexpected
                    close(fd);
                    removeFdFromPoll(fd);
                }
            }

            if (fds[i].revents & (POLLHUP | POLLERR)) {
                // Disconnection or error
                if (clients.find(fd) != clients.end()) {
                    handleDisconnection(fd);
                } else {
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
        client.expect_100_continue = false;
        client.file_fd = -1;
        client.reading_file = false;
        client.file_path = "";
        client.file_bytes_read = 0;
        client.file_size = 0;
        client.upload_fd = -1;
        client.writing_file = false;
        client.file_bytes_written = 0;

        clients[new_socket] = client;
        client_server_map[new_socket] = std::find(server_fds.begin(), server_fds.end(), server_fd) - server_fds.begin();
    }
}

void WebServer::handleClientRead(int client_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t valread = recv(client_fd, buffer, sizeof(buffer), 0);

    if (valread > 0) {
        ClientConnection &client = clients[client_fd];
        client.request.append(buffer, valread);

        if (!client.headers_received) {
            size_t header_end = client.request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                parseRequestHeaders(client);
                client.request = client.request.substr(header_end + 4);
                if (client.expect_100_continue) {
                    // ignore sending 100 continue (no direct write), just proceed
                    client.expect_100_continue = false;
                }
            }
        }

        if (client.headers_received) {
            if (client.chunked_encoding) {
                processChunkedRequest(client);
            } else if (client.content_length > 0) {
                client.body += client.request;
                client.request.clear();
                if (client.body.size() >= client.content_length) {
                    client.body = client.body.substr(0, client.content_length);
                    processRequest(client_fd);
                }
            } else {
                // no body expected
                processRequest(client_fd);
            }
        }
    } else if (valread == 0) {
        // client closed connection
        handleDisconnection(client_fd);
    } else {
        // error reading
        handleDisconnection(client_fd);
    }
}

void WebServer::handleClientWrite(int client_fd) {
    ClientConnection &client = clients[client_fd];

    if (!client.response_ready) {
        return;
    }

    size_t to_send = client.response.size() - client.response_sent;
    if (to_send > 0) {
        ssize_t sent = send(client_fd, client.response.c_str() + client.response_sent, to_send, 0);
        if (sent > 0) {
            client.response_sent += sent;
            if (client.response_sent >= client.response.size()) {
                handleDisconnection(client_fd);
            }
        } else if (sent == 0) {
            handleDisconnection(client_fd);
        } else {
            handleDisconnection(client_fd);
        }
    } else {
        handleDisconnection(client_fd);
    }
}

void WebServer::handleFileRead(int file_fd) {
    ClientConnection *client = findClientByFileFD(file_fd);
    if (!client) {
        close(file_fd);
        removeFdFromPoll(file_fd);
        return;
    }

    char buffer[BUFFER_SIZE];
    ssize_t nbytes = read(file_fd, buffer, sizeof(buffer));

    if (nbytes > 0) {
        client->response.append(buffer, nbytes);
        client->file_bytes_read += nbytes;
        if (client->file_bytes_read >= client->file_size) {
            // done reading file
            close(file_fd);
            removeFdFromPoll(file_fd);
            client->reading_file = false;
            client->file_fd = -1;

            // prepend headers now
            std::string content_type = getMimeType(client->file_path);
            std::ostringstream response_stream;
            response_stream << "HTTP/1.1 200 OK\r\n";
            response_stream << "Content-Type: " << content_type << "\r\n";
            response_stream << "Content-Length: " << client->response.size() << "\r\n";
            response_stream << "Connection: close\r\n\r\n";
            client->response = response_stream.str() + client->response;

            client->response_ready = true;

            // Add POLLOUT for client fd
            for (int j = 0; j < nfds; ++j) {
                if (fds[j].fd == client->fd) {
                    fds[j].events |= POLLOUT;
                    break;
                }
            }
        } else {
            // not done reading yet, wait for next poll iteration
        }
    } else if (nbytes == 0) {
        // EOF early
        close(file_fd);
        removeFdFromPoll(file_fd);
        client->reading_file = false;
        client->file_fd = -1;
        sendErrorResponse(*client, 500);
    } else {
        // error reading file
        close(file_fd);
        removeFdFromPoll(file_fd);
        sendErrorResponse(*client, 500);
    }
}

void WebServer::handleFileWrite(int file_fd) {
    ClientConnection *client = findClientByUploadFD(file_fd);
    if (!client) {
        close(file_fd);
        removeFdFromPoll(file_fd);
        return;
    }

    size_t remaining = client->body.size() - client->file_bytes_written;
    if (remaining > 0) {
        ssize_t written = write(file_fd, client->body.c_str() + client->file_bytes_written, remaining);
        if (written > 0) {
            client->file_bytes_written += written;
            if (client->file_bytes_written == client->body.size()) {
                // done writing
                close(file_fd);
                removeFdFromPoll(file_fd);
                client->writing_file = false;
                client->upload_fd = -1;

                // send success response
                std::string response_body = "<html><body><h1>File Uploaded Successfully</h1></body></html>";
                std::ostringstream oss;
                oss << "HTTP/1.1 200 OK\r\n";
                oss << "Content-Type: text/html\r\n";
                oss << "Content-Length: " << response_body.size() << "\r\n";
                oss << "Connection: close\r\n\r\n";
                oss << response_body;
                client->response = oss.str();
                client->response_ready = true;

                for (int j = 0; j < nfds; ++j) {
                    if (fds[j].fd == client->fd) {
                        fds[j].events |= POLLOUT;
                        break;
                    }
                }
            } else {
                // not finished writing yet
            }
        } else if (written == 0) {
            // connection closed?
            close(file_fd);
            removeFdFromPoll(file_fd);
            handleDisconnection(client->fd);
        } else {
            // error writing
            close(file_fd);
            removeFdFromPoll(file_fd);
            handleDisconnection(client->fd);
        }
    } else {
        // no data to write, strange
        close(file_fd);
        removeFdFromPoll(file_fd);
        sendErrorResponse(*client, 500);
    }
}

void WebServer::parseRequestHeaders(ClientConnection &client) {
    std::string request = client.request;
    size_t headers_end = request.find("\r\n\r\n");
    std::string headers = request.substr(0, headers_end);
    std::istringstream request_stream(headers);
    std::string line;

    std::getline(request_stream, line);
    {
        std::istringstream request_line_stream(line);
        request_line_stream >> client.method >> client.url >> client.http_version;
    }

    while (std::getline(request_stream, line) && line != "\r") {
        size_t pos = line.find(": ");
        if (pos != std::string::npos) {
            std::string header_name = line.substr(0, pos);
            std::string header_value = line.substr(pos + 2);
            if (!header_value.empty() && header_value[header_value.size() - 1] == '\r') {
                header_value = header_value.substr(0, header_value.size() - 1);
            }

            // Log the header name and value
            std::cout << "Header: " << header_name << " = " << header_value << std::endl;

            client.headers[header_name] = header_value;

            if (header_name == "Content-Length") {
                client.content_length = std::atoi(header_value.c_str());
            } else if (header_name == "Content-Type") {
                client.content_type = header_value;
            } else if (header_name == "Transfer-Encoding") {
                client.transfer_encoding = header_value;
                if (header_value == "chunked") {
                    client.chunked_encoding = true;
                }
            } else if (header_name == "Expect" && header_value == "100-continue") {
                client.expect_100_continue = true;
            }
        }
    }

    size_t pos = client.url.find('?');
    if (pos != std::string::npos) {
        client.query_string = client.url.substr(pos + 1);
        client.url = client.url.substr(0, pos);
    }

    client.headers_received = true;
}

bool WebServer::isRequestComplete(ClientConnection &client) {
    size_t headers_end = client.request.find("\r\n\r\n");
    if (headers_end == std::string::npos) {
        return false;
    }

    size_t body_start = headers_end + 4;
    size_t request_size = client.request.size();

    if (client.chunked_encoding) {
        return false; 
    } else if (client.content_length > 0) {
        size_t total_size = body_start + client.content_length;
        return request_size >= total_size;
    } else {
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
                return;
            }
            std::string chunk_size_str = request.substr(0, pos);
            client.current_chunk_size = strtoul(chunk_size_str.c_str(), NULL, 16);
            client.chunk_size_received = true;
            request.erase(0, pos + 2);
        }

        if (client.current_chunk_size == 0) {
            client.chunked_encoding = false;
            processRequest(client.fd);
            return;
        }

        if (request.size() < client.current_chunk_size + 2) {
            return;
        }

        body.append(request.substr(0, client.current_chunk_size));
        request.erase(0, client.current_chunk_size + 2);
        client.chunk_size_received = false;
    }
}

void WebServer::processRequest(int client_fd) {
    ClientConnection &client = clients[client_fd];
    const ServerConfig &server_config = configs[client_server_map[client_fd]];

    if (client.content_length > 0) {
        if (client.body.size() < client.content_length) {
            return;
        }
    }

    // find matching route
    const RouteConfig *best_match_route = NULL;
    size_t longest_match_length = 0;

    for (size_t i = 0; i < server_config.routes.size(); ++i) {
        const RouteConfig &route = server_config.routes[i];
        if ((route.url == "/" && client.url.find(route.url) == 0) ||
            (route.url != "/" && (client.url == route.url || client.url.find(route.url + "/") == 0))) {
            if (route.url.size() >= longest_match_length) {
                best_match_route = &route;
                longest_match_length = route.url.size();
            }
        }
    }

    for (size_t i = 0; i < server_config.routes.size(); ++i) {
        const RouteConfig &route = server_config.routes[i];
        if (!route.url.empty() && route.url[0] == '.') {
            size_t ext_length = route.url.size();
            if (client.url.size() >= ext_length &&
                client.url.compare(client.url.size() - ext_length, ext_length, route.url) == 0) {
                best_match_route = &route;
            }
        }
    }

    if (!best_match_route) {
        sendErrorResponse(client, 404);
        return;
    }

    client.route = best_match_route;
    const RouteConfig &route = *best_match_route;

    if (std::find(route.methods.begin(), route.methods.end(), client.method) == route.methods.end()) {
        std::string allowed_methods;
        for (size_t i = 0; i < route.methods.size(); ++i) {
            allowed_methods += route.methods[i];
            if (i < route.methods.size() - 1) {
                allowed_methods += ", ";
            }
        }
        sendErrorResponse(client, 405, "Allow: " + allowed_methods + "\r\n");
        return;
    }

    if (client.method == "GET") {
        handleGetRequest(client);
    } else if (client.method == "POST") {
        if (route.max_body > 0 && client.body.size() > route.max_body) {
            sendErrorResponse(client, 413);
            return;
        }
        handlePostRequest(client);
    } else if (client.method == "DELETE") {
        handleDeleteRequest(client);
    } else if (client.method == "PUT") {
        handlePutRequest(client);
    } else {
        sendErrorResponse(client, 501);
    }

    // Reset request state (for simplicity)
    client.headers_received = false;
    client.request.clear();
    client.method.clear();
    client.url.clear();
    client.http_version.clear();
    client.content_type.clear();
    client.query_string.clear();
    client.transfer_encoding.clear();
    client.chunked_encoding = false;
    client.current_chunk_size = 0;
    client.chunk_size_received = false;
    client.content_length = 0;
}

void WebServer::handleGetRequest(ClientConnection &client) {
    const RouteConfig &route = *client.route;
    if (!route.cgi_path.empty()) {
        handleCGI(client.fd);
        return;
    }

    std::string file_path = constructFilePath(route, client.url);
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) == -1) {
        sendErrorResponse(client, 404);
        return;
    }

    if (S_ISDIR(file_stat.st_mode)) {
        handleDirectoryRequest(client, file_path);
    } else {
        prepareFileResponse(client, file_path);
    }
}

void WebServer::handlePostRequest(ClientConnection &client) {
    const RouteConfig &route = *client.route;

    if (client.content_type.find("multipart/form-data") != std::string::npos ||
        (!route.upload_dir.empty() && client.method == "POST")) {
        // handle file upload
        handleFileUpload(client);
    } else if (!route.cgi_path.empty()) {
        handleCGI(client.fd);
    } else {
        // just respond 200 OK no content
        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.response = response;
        client.response_ready = true;

        for (int i = 0; i < nfds; ++i) {
            if (fds[i].fd == client.fd) {
                fds[i].events |= POLLOUT;
                break;
            }
        }
    }
}

void WebServer::handleDeleteRequest(ClientConnection &client) {
    const RouteConfig &route = *client.route;

    // Parse filename from query_string
    std::string filename;
    size_t pos = client.query_string.find("file=");
    if (pos == std::string::npos) {
        // No 'file=' parameter in query string
        sendErrorResponse(client, 400); // Bad Request
        return;
    }

    // Extract the filename from the query string
    filename = client.query_string.substr(pos + 5); // 'file=' is 5 chars
    if (filename.empty()) {
        // 'file=' was found but no filename provided
        sendErrorResponse(client, 400);
        return;
    }

    // Construct the full file path
    std::string file_path = route.upload_dir + "/" + filename;

    // Attempt to remove the file
    if (remove(file_path.c_str()) == 0) {
        // File removed successfully
        std::string response = "HTTP/1.1 200 OK\r\n"
                               "Content-Length: 0\r\n"
                               "Connection: close\r\n\r\n";
        client.response = response;
        client.response_ready = true;

        // Add POLLOUT event for the client's fd so the response is sent
        for (int i = 0; i < nfds; ++i) {
            if (fds[i].fd == client.fd) {
                fds[i].events |= POLLOUT;
                break;
            }
        }
    } else {
        // File not found or could not be removed
        sendErrorResponse(client, 404);
    }
}

void WebServer::handlePutRequest(ClientConnection &client) {
    const RouteConfig &route = *client.route;
    std::string upload_path = route.upload_dir + client.url.substr(route.url.size());

    // create directories if needed
    size_t last_slash_pos = upload_path.find_last_of('/');
    if (last_slash_pos != std::string::npos) {
        std::string dir_path = upload_path.substr(0, last_slash_pos);
        mkdir(dir_path.c_str(), 0755);
    }

    prepareFileUpload(client, upload_path);
}

void WebServer::handleDirectoryRequest(ClientConnection &client, const std::string &dir_path) {
    const RouteConfig &route = *client.route;
    std::string index = route.index.empty() ? "index.html" : route.index;

    {
        struct stat index_stat;
        std::string index_path = dir_path + "/" + index;
        if (!index.empty() && stat(index_path.c_str(), &index_stat) == 0) {
            // Serve the index file
            prepareFileResponse(client, index_path);
            return;
        }
    }

    if (route.autoindex) {
        std::string listing = generateDirectoryListing(dir_path, client.url);
        std::ostringstream response_stream;
        response_stream << "HTTP/1.1 200 OK\r\n";
        response_stream << "Content-Type: text/html\r\n";
        response_stream << "Content-Length: " << listing.size() << "\r\n";
        response_stream << "Connection: close\r\n\r\n";
        response_stream << listing;
        client.response = response_stream.str();
        client.response_ready = true;

        for (int i = 0; i < nfds; ++i) {
            if (fds[i].fd == client.fd) {
                fds[i].events |= POLLOUT;
                break;
            }
        }
    } else {
        sendErrorResponse(client, 404);
    }
}

void WebServer::prepareFileResponse(ClientConnection &client, const std::string &file_path) {
    // open file non-blocking
    int fd = open(file_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        sendErrorResponse(client, 403);
        return;
    }

    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) == -1) {
        close(fd);
        sendErrorResponse(client, 404);
        return;
    }

    client.file_fd = fd;
    client.reading_file = true;
    client.file_path = file_path;
    client.file_size = (size_t)file_stat.st_size;
    client.file_bytes_read = 0;

    addFdToPoll(fd, POLLIN);
}

void WebServer::prepareFileUpload(ClientConnection &client, const std::string &upload_path) {
    int fd = open(upload_path.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK, 0644);
    if (fd < 0) {
        sendErrorResponse(client, 500);
        return;
    }

    client.upload_fd = fd;
    client.writing_file = true;
    client.file_bytes_written = 0;

    addFdToPoll(fd, POLLOUT);
}

void WebServer::handleFileUpload(ClientConnection &client) {
    const RouteConfig &route = *client.route;

    if (client.body.empty()) {
        sendErrorResponse(client, 400);
        return;
    }

    std::string filename;
    std::string file_content;

    // Check if multipart/form-data
    size_t boundary_pos;
    std::string boundary;
    if ((boundary_pos = client.content_type.find("boundary=")) != std::string::npos) {
        // Parse out the boundary
        boundary = "--" + client.content_type.substr(boundary_pos + 9); // 9 = length of "boundary="
    }

    if (!boundary.empty()) {
        // Multipart form-data parsing
        // Example of structure:
        // --boundary\r\n
        // Content-Disposition: form-data; name="file"; filename="Balance sheet for Ihor Paslavskyy.pdf"\r\n
        // Content-Type: application/pdf\r\n
        // \r\n
        // ... file bytes ...
        // \r\n--boundary--\r\n

        // Find the first boundary occurrence
        size_t start = client.body.find(boundary);
        if (start == std::string::npos) {
            sendErrorResponse(client, 400);
            return;
        }

        // Move past the boundary line
        start += boundary.size();
        if (client.body.compare(start, 2, "\r\n") == 0) {
            start += 2;
        }

        // Now we are at the start of the part headers
        size_t header_end = client.body.find("\r\n\r\n", start);
        if (header_end == std::string::npos) {
            sendErrorResponse(client, 400);
            return;
        }

        std::string part_headers = client.body.substr(start, header_end - start);

        // Extract filename from Content-Disposition
        // Content-Disposition: form-data; name="file"; filename="Balance sheet for Ihor Paslavskyy.pdf"
        {
            size_t disp_pos = part_headers.find("Content-Disposition:");
            if (disp_pos != std::string::npos) {
                size_t fn_pos = part_headers.find("filename=\"", disp_pos);
                if (fn_pos != std::string::npos) {
                    fn_pos += 10; // skip filename="
                    size_t fn_end = part_headers.find("\"", fn_pos);
                    if (fn_end != std::string::npos) {
                        filename = part_headers.substr(fn_pos, fn_end - fn_pos);
                    }
                }
            }
        }

        // The file content starts after \r\n\r\n
        size_t content_start = header_end + 4;

        // Locate the ending boundary for this file part
        // The part ends at the next boundary line which starts with \r\n--boundary
        std::string end_boundary_seq = "\r\n" + boundary;
        size_t end_boundary_pos = client.body.find(end_boundary_seq, content_start);
        if (end_boundary_pos == std::string::npos) {
            // Could not find the ending boundary, malformed request
            sendErrorResponse(client, 400);
            return;
        }

        // Extract the file content only
        file_content = client.body.substr(content_start, end_boundary_pos - content_start);

        if (filename.empty()) {
            // No filename provided by client, fallback to a generated name
            filename = "uploaded_" + intToString((int)time(NULL));
        }

    } else {
        // Not multipart/form-data, treat entire body as file content
        filename = "uploaded_" + intToString((int)time(NULL));
        file_content = client.body;
    }

    // Now we have the exact file content and the actual filename
    std::string filepath = route.upload_dir + "/" + filename;

    // Open the file in non-blocking mode for writing
    int fd = open(filepath.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK, 0644);
    if (fd < 0) {
        sendErrorResponse(client, 500);
        return;
    }

    // Store the file content in client's body for writing chunk by chunk
    client.upload_fd = fd;
    client.writing_file = true;
    client.file_bytes_written = 0;
    client.body = file_content;

    // Add the file descriptor to poll for POLLOUT to start writing
    addFdToPoll(fd, POLLOUT);
}

void WebServer::handleCGI(int client_fd) {
    ClientConnection &client = clients[client_fd];
    const RouteConfig &route = *client.route;

    int cgi_output[2];
    int cgi_input[2];

    if (pipe(cgi_output) < 0 || pipe(cgi_input) < 0) {
        sendErrorResponse(client, 500);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        sendErrorResponse(client, 500);
        return;
    }

    if (pid == 0) {
        close(cgi_output[0]);
        close(cgi_input[1]);

        dup2(cgi_output[1], STDOUT_FILENO);
        dup2(cgi_input[0], STDIN_FILENO);

        close(cgi_output[1]);
        close(cgi_input[0]);

        extern char **environ;

        std::string script_name = client.url.substr(client.url.find_last_of('/') + 1);

        setenv("REQUEST_METHOD", client.method.c_str(), 1);
        setenv("QUERY_STRING", client.query_string.c_str(), 1);
        setenv("CONTENT_LENGTH", intToString((int)client.body.size()).c_str(), 1);
        setenv("CONTENT_TYPE", client.content_type.c_str(), 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("PATH_INFO", script_name.c_str(), 1);
        std::string path_translated = route.root + client.url;
        setenv("PATH_TRANSLATED", path_translated.c_str(), 1);
        setenv("SERVER_NAME", "localhost", 1);
        setenv("SERVER_PORT", intToString(configs[client_server_map[client.fd]].port).c_str(), 1);
        setenv("REMOTE_ADDR", "127.0.0.1", 1);
        setenv("REDIRECT_STATUS", "200", 1);

        std::map<std::string, std::string>::iterator secret_it = client.headers.find("X-Secret-Header-For-Test");
        if (secret_it != client.headers.end()) {
            setenv("HTTP_X_SECRET_HEADER_FOR_TEST", secret_it->second.c_str(), 1);
        }


        std::string script_dir = route.cgi_path.substr(0, route.cgi_path.find_last_of("/"));
        if (!script_dir.empty()) {
            chdir(script_dir.c_str());
        }

        char *args[] = {const_cast<char *>(route.cgi_path.c_str()), NULL};
        execve(route.cgi_path.c_str(), args, environ);
        exit(EXIT_FAILURE);
    } else {
        close(cgi_output[1]);
        close(cgi_input[0]);

        fcntl(cgi_output[0], F_SETFL, O_NONBLOCK);
        fcntl(cgi_input[1], F_SETFL, O_NONBLOCK);

        client.cgi_pid = pid;
        client.cgi_input_fd = cgi_input[1];
        client.cgi_output_fd = cgi_output[0];
        client.total_cgi_input_written = 0;
        client.cgi_output = "";

        addFdToPoll(client.cgi_input_fd, POLLOUT);
        addFdToPoll(client.cgi_output_fd, POLLIN);
    }
}

void WebServer::handleCGIWrite(int cgi_input_fd) {
    ClientConnection *client = findClientByCGIFD(cgi_input_fd);
    if (!client) {
        close(cgi_input_fd);
        removeFdFromPoll(cgi_input_fd);
        return;
    }

    size_t remaining = client->body.size() - client->total_cgi_input_written;
    if (remaining > 0) {
        ssize_t written = write(cgi_input_fd, client->body.c_str() + client->total_cgi_input_written, remaining);
        if (written > 0) {
            client->total_cgi_input_written += written;
            if (client->total_cgi_input_written == client->body.size()) {
                close(cgi_input_fd);
                client->cgi_input_fd = -1;
                removeFdFromPoll(cgi_input_fd);
            }
        } else if (written == 0) {
            close(cgi_input_fd);
            client->cgi_input_fd = -1;
            removeFdFromPoll(cgi_input_fd);
            handleDisconnection(client->fd);
        } else {
            close(cgi_input_fd);
            client->cgi_input_fd = -1;
            removeFdFromPoll(cgi_input_fd);
            handleDisconnection(client->fd);
        }
    } else {
        close(cgi_input_fd);
        client->cgi_input_fd = -1;
        removeFdFromPoll(cgi_input_fd);
    }
}

void WebServer::handleCGIRead(int cgi_output_fd) {
    ClientConnection *client = findClientByCGIFD(cgi_output_fd);
    if (!client) {
        close(cgi_output_fd);
        removeFdFromPoll(cgi_output_fd);
        return;
    }

    char buffer[BUFFER_SIZE];
    ssize_t nbytes = read(cgi_output_fd, buffer, sizeof(buffer));

    if (nbytes > 0) {
        client->cgi_output.append(buffer, nbytes);
    } else if (nbytes == 0) {
        close(cgi_output_fd);
        client->cgi_output_fd = -1;
        removeFdFromPoll(cgi_output_fd);

        if (client->cgi_pid != -1) {
            int status;
            waitpid(client->cgi_pid, &status, 0);
            client->cgi_pid = -1;
        }

        processCGIOutput(*client);
        client->response_ready = true;

        for (int j = 0; j < nfds; ++j) {
            if (fds[j].fd == client->fd) {
                fds[j].events |= POLLOUT;
                break;
            }
        }

    } else {
        close(cgi_output_fd);
        client->cgi_output_fd = -1;
        removeFdFromPoll(cgi_output_fd);

        sendErrorResponse(*client, 500);
        client->response_ready = true;

        for (int j = 0; j < nfds; ++j) {
            if (fds[j].fd == client->fd) {
                fds[j].events |= POLLOUT;
                break;
            }
        }
    }
}

void WebServer::processCGIOutput(ClientConnection &client) {
    if (client.cgi_output.empty()) {
        sendErrorResponse(client, 500);
        return;
    }

    size_t header_end = client.cgi_output.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        sendErrorResponse(client, 500);
        return;
    }

    std::string headers = client.cgi_output.substr(0, header_end);
    std::string body = client.cgi_output.substr(header_end + 4);

    bool has_content_type = false;
    {
        std::istringstream header_stream(headers);
        std::string line;
        while (std::getline(header_stream, line)) {
            if (!line.empty() && line[line.size() - 1] == '\r') {
                line.erase(line.size() - 1);
            }
            if (line.find("Content-Type:") != std::string::npos) {
                has_content_type = true;
                break;
            }
        }
    }

    std::ostringstream response_stream;
    response_stream << "HTTP/1.1 200 OK\r\n";
    if (!has_content_type) {
        response_stream << "Content-Type: text/html\r\n";
    }

    response_stream << headers << "\r\n";
    response_stream << "Content-Length: " << body.size() << "\r\n";
    response_stream << "Connection: close\r\n\r\n";
    response_stream << body;

    client.response = response_stream.str();
    client.response_ready = true;
}

void WebServer::sendErrorResponse(ClientConnection &client, int status_code, const std::string &additional_headers) {
    std::string error_body = getErrorPageContent(status_code);
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

    for (int i = 0; i < nfds; ++i) {
        if (fds[i].fd == client.fd) {
            fds[i].events |= POLLOUT;
            break;
        }
    }
}

void WebServer::handleDisconnection(int fd) {
    std::map<int, ClientConnection>::iterator it = clients.find(fd);
    if (it != clients.end()) {
        ClientConnection &client = it->second;
        close(fd);
        removeFdFromPoll(fd);

        if (client.cgi_input_fd != -1) {
            close(client.cgi_input_fd);
            removeFdFromPoll(client.cgi_input_fd);
        }

        if (client.cgi_output_fd != -1) {
            close(client.cgi_output_fd);
            removeFdFromPoll(client.cgi_output_fd);
        }

        if (client.file_fd != -1) {
            close(client.file_fd);
            removeFdFromPoll(client.file_fd);
        }

        if (client.upload_fd != -1) {
            close(client.upload_fd);
            removeFdFromPoll(client.upload_fd);
        }

        if (client.cgi_pid != -1) {
            kill(client.cgi_pid, SIGKILL);
            waitpid(client.cgi_pid, NULL, 0);
        }

        clients.erase(it);
    } else {
        close(fd);
        removeFdFromPoll(fd);
    }
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

std::string WebServer::getDefaultErrorPage(int status_code) {
    std::ostringstream oss;
    oss << "<!DOCTYPE html><html><head><title>"
        << status_code << " " << getStatusText(status_code)
        << "</title></head><body><h1>" << status_code << " "
        << getStatusText(status_code) << "</h1></body></html>";
    return oss.str();
}

std::string WebServer::getErrorPageContent(int status_code) {
    // We can just return a default error page for all codes
    return getDefaultErrorPage(status_code);
}

std::string WebServer::constructFilePath(const RouteConfig &route, const std::string &url) {
    std::string file_path;
    std::string remaining_url = url.substr(route.url.size());
    if (!route.alias.empty()) {
        file_path = route.alias;
    } else {
        file_path = route.root;
    }

    if (!remaining_url.empty() && remaining_url[0] == '/') {
        remaining_url = remaining_url.substr(1);
    }

    if (!file_path.empty() && file_path[file_path.size() - 1] != '/' && !remaining_url.empty()) {
        file_path += "/";
    }
    file_path += remaining_url;
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
            if (!entry_url.empty() && entry_url[entry_url.size() - 1] != '/') {
                entry_url += "/";
            }
            entry_url += entry_name;
            listing << "<li><a href=\"" << entry_url << "\">" << entry_name << "</a></li>";
        }
        closedir(dir);
    } else {
        return "<html><body><h1>Error listing directory</h1></body></html>";
    }

    listing << "</ul></body></html>";
    return listing.str();
}

std::string WebServer::getMimeType(const std::string &path) {
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) {
        return "application/octet-stream";
    }
    std::string extension = path.substr(pos + 1);
    if (extension == "html" || extension == "htm") return "text/html";
    if (extension == "css") return "text/css";
    if (extension == "js") return "application/javascript";
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "png") return "image/png";
    if (extension == "gif") return "image/gif";
    return "application/octet-stream";
}

std::string WebServer::intToString(int value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}
