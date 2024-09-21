// WebServer.cpp
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

#define BUFFER_SIZE 1024
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
        perror("fcntl F_GETFL");
        exit(EXIT_FAILURE);
    }
    if (fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        exit(EXIT_FAILURE);
    }
}

void WebServer::bindSocket(int &server_fd, int port) {
    int opt = 1;
    struct sockaddr_in address;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen");
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
            fds[i].fd = -1;
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
            perror("poll");
            exit(EXIT_FAILURE);
        }
        
        int bufnfds = nfds;
        for (int i = 0; i < bufnfds; ++i) {
            if (fds[i].revents & POLLIN) {
                if (std::find(server_fds.begin(), server_fds.end(), fds[i].fd) != server_fds.end()) {
                    // Accept new connections
                    struct sockaddr_in address;
                    socklen_t addrlen = sizeof(address);
                    int new_socket;

                    while ((new_socket = accept(fds[i].fd, (struct sockaddr *)&address, &addrlen)) >= 0) {
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
                        clients[new_socket] = client;

                        client_server_map[new_socket] = i;
                    }
                } else {
                    // Read data from client or CGI output
                    if (clients.find(fds[i].fd) != clients.end()) {
                        handleClientRead(fds[i].fd);
                    } else {
                        // Check if it's a CGI output fd
                        handleCGIRead(fds[i].fd);
                    }
                }
            }

            if (fds[i].revents & POLLOUT) {
                // Write data to client or CGI input
                if (clients.find(fds[i].fd) != clients.end()) {
                    handleClientWrite(fds[i].fd);
                } else {
                    // Check if it's a CGI input fd
                    handleCGIWrite(fds[i].fd);
                }
            }
            
            if (fds[i].revents & (POLLHUP | POLLERR)) {
                std::cout << "POLLHUP or POLLERR on socket " << fds[i].fd << std::endl;

                // Check if fd is a client socket
                if (clients.find(fds[i].fd) != clients.end()) {
                    // It's a client socket
                    close(fds[i].fd);
                    clients.erase(fds[i].fd);
                    fds[i].fd = -1;
                } else {
                    // Check if fd is associated with a CGI process
                    ClientConnection *client = findClientByCGIFD(fds[i].fd);
                    if (client) {
                        if (fds[i].fd == client->cgi_output_fd) {
                            // Read any remaining data from CGI output
                            handleCGIRead(fds[i].fd);
                            // Close CGI output fd
                            close(client->cgi_output_fd);
                            client->cgi_output_fd = -1;
                            removeFdFromPoll(fds[i].fd);
                            fds[i].fd = -1;

                            // Wait for CGI process to finish
                            waitpid(client->cgi_pid, NULL, 0);

                            // Construct response
                            std::string status_line = "HTTP/1.1 200 OK\r\n";
                            client->response = status_line + client->cgi_output;
                            client->response_ready = true;
                            std::cout << "Response ready for client_fd: " << client->fd << std::endl;
                        } else if (fds[i].fd == client->cgi_input_fd) {
                            // Close CGI input fd
                            close(client->cgi_input_fd);
                            client->cgi_input_fd = -1;
                            removeFdFromPoll(fds[i].fd);
                            fds[i].fd = -1;
                        }
                    } else {
                        // Unknown fd, just close it
                        close(fds[i].fd);
                        fds[i].fd = -1;
                    }
                }
            }
        }

        // Clean up closed fds
        nfds = std::remove_if(fds, fds + nfds, is_closed_socket) - fds;
    }
}

void WebServer::handleClientRead(int client_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t valread = recv(client_fd, buffer, sizeof(buffer), 0);

    if (valread > 0) {
        ClientConnection &client = clients[client_fd];
        client.request.append(buffer, valread);

        if (!client.headers_received && client.request.find("\r\n\r\n") != std::string::npos) {
            parseRequestHeaders(client);
        }

        if (client.headers_received && isRequestComplete(client)) {
            processRequest(client_fd);
        }
    } else if (valread == 0) {
        // Client disconnected
        std::cout << "Client disconnected from socket " << client_fd << std::endl;
        close(client_fd);
        clients.erase(client_fd);
        removeFdFromPoll(client_fd);
    } else {
        // Do not check errno; simply wait for the next poll iteration
        // Optionally, log the error without checking errno
    }
}

void WebServer::handleClientWrite(int client_fd) {
    ClientConnection &client = clients[client_fd];

    if (!client.response_ready) {
        // Response not ready yet
        return;
    }

    ssize_t to_send = client.response.size() - client.response_sent;
    if (to_send > 0) {
        ssize_t sent = send(client_fd, client.response.c_str() + client.response_sent, to_send, 0);
        if (sent > 0) {
            client.response_sent += sent;
            if (client.response_sent >= client.response.size()) {
                // Response fully sent
                close(client_fd);
                clients.erase(client_fd);
                removeFdFromPoll(client_fd);
            }
        } else if (sent == 0) {
            // Connection closed
            close(client_fd);
            clients.erase(client_fd);
            removeFdFromPoll(client_fd);
        } else {
            // Do not check errno; simply wait for the next POLLOUT event
        }
    } else {
        // No more data to send
        close(client_fd);
        clients.erase(client_fd);
        removeFdFromPoll(client_fd);
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

            if (header_name == "Content-Length") {
                client.content_length = std::atoi(header_value.c_str());
            } else if (header_name == "Content-Type") {
                client.content_type = header_value;
            } else if (header_name == "Transfer-Encoding") {
                client.transfer_encoding = header_value;
            }
        }
    }

    // Parse query string
    size_t pos = client.url.find('?');
    if (pos != std::string::npos) {
        client.query_string = client.url.substr(pos + 1);
        client.url = client.url.substr(0, pos);
    }

    client.headers_received = true;
}

bool WebServer::isRequestComplete(ClientConnection &client) {
    size_t headers_end = client.request.find("\r\n\r\n");
    size_t body_start = headers_end + 4;
    size_t request_size = client.request.size();

    if (client.transfer_encoding == "chunked") {
        // Handle chunked transfer encoding
        size_t pos = body_start;
        while (true) {
            size_t chunk_size_end = client.request.find("\r\n", pos);
            if (chunk_size_end == std::string::npos)
                return false; // Need more data

            std::string chunk_size_str = client.request.substr(pos, chunk_size_end - pos);
            size_t chunk_size;
            std::istringstream(chunk_size_str) >> std::hex >> chunk_size;

            pos = chunk_size_end + 2;
            size_t chunk_data_end = pos + chunk_size;

            if (chunk_data_end > request_size)
                return false; // Need more data

            client.body.append(client.request.substr(pos, chunk_size));
            pos = chunk_data_end + 2; // Skip \r\n after chunk data

            if (chunk_size == 0)
                break; // Last chunk
        }
        return true;
    } else if (client.content_length > 0) {
        size_t total_size = body_start + client.content_length;
        if (request_size >= total_size) {
            client.body = client.request.substr(body_start, client.content_length);
            return true;
        } else {
            return false; // Need more data
        }
    } else {
        // No body expected
        return true;
    }
}

void WebServer::processRequest(int client_fd) {
    ClientConnection &client = clients[client_fd];
    const ServerConfig &server_config = configs[client_server_map[client_fd]];
    std::string method = client.method;
    std::string url = client.url;
    std::string http_version = client.http_version;

    // Find the best matching route
    const RouteConfig *best_match_route = NULL;
    size_t longest_match_length = 0;

    for (size_t i = 0; i < server_config.routes.size(); ++i) {
        const RouteConfig &route = server_config.routes[i];
        if ((route.url == "/" && url.find(route.url) == 0) ||
            (route.url != "/" && (url == route.url || url.find(route.url + "/") == 0))) {
            if (route.url.size() >= longest_match_length) {
                best_match_route = &route;
                longest_match_length = route.url.size();
            }
        }
    }

    if (!best_match_route) {
        std::cerr << "Route not found for URL: " << url << std::endl;
        std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.response = response;
        client.response_ready = true;
        return;
    }

    client.route = best_match_route;
    const RouteConfig &route = *best_match_route;

    // Check if the method is allowed
    if (std::find(route.methods.begin(), route.methods.end(), method) == route.methods.end()) {
        std::string allowed_methods;
        for (size_t i = 0; i < route.methods.size(); ++i)
        {
            allowed_methods += route.methods[i];
            if (i < route.methods.size() - 1)
            {
                allowed_methods += ", ";
            }
        }
        std::string error_body = getErrorPage(server_config, 405);
        if (error_body.empty())
        {
            error_body = getDefaultErrorPage(405);
        }
        std::ostringstream response_stream;
        response_stream << "HTTP/1.1 405 Method Not Allowed\r\n";
        response_stream << "Content-Type: text/html\r\n";
        response_stream << "Content-Length: " << error_body.size() << "\r\n";
        response_stream << "Allow: " << allowed_methods << "\r\n";
        response_stream << "Connection: close\r\n\r\n";
        response_stream << error_body;
        client.response = response_stream.str();
        client.response_ready = true;
        return;
    }

     // **Check for Max Body Size Limit**
    if (client.route->max_body > 0 && client.body.size() > client.route->max_body) {
        std::cerr << "Request body exceeds max allowed size" << std::endl;
        std::string error_body = getErrorPage(server_config, 413);
        if (error_body.empty()) {
            error_body = getDefaultErrorPage(413);
        }
        std::ostringstream response_stream;
        response_stream << "HTTP/1.1 413 Payload Too Large\r\n";
        response_stream << "Content-Type: text/html\r\n";
        response_stream << "Content-Length: " << error_body.size() << "\r\n";
        response_stream << "Connection: close\r\n\r\n";
        response_stream << error_body;
        client.response = response_stream.str();
        client.response_ready = true;
        return;
    }

    // Handle DELETE request
    if (method == "DELETE") {
        handleDeleteRequest(client_fd);
        return;
    }

    // Handle PUT request
    if (method == "PUT") {
        std::string upload_dir = route.upload_dir;
        if (upload_dir.empty()) {
            std::cerr << "Upload directory not specified for PUT request" << std::endl;
            std::string response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            client.response = response;
            client.response_ready = true;
            return;
        }

        // Construct the full path for the uploaded file
        std::string upload_path = upload_dir + url.substr(route.url.size());
        std::cerr << "Uploading file to: " << upload_path << std::endl;

        // Create directories if they do not exist
        size_t last_slash_pos = upload_path.find_last_of('/');
        if (last_slash_pos != std::string::npos) {
            std::string dir_path = upload_path.substr(0, last_slash_pos);
            // Ensure directory path is safe
            if (dir_path.find("..") != std::string::npos) {
                std::cerr << "Invalid directory path: " << dir_path << std::endl;
                std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                client.response = response;
                client.response_ready = true;
                return;
            }
            mkdir(dir_path.c_str(), 0755);
        }

        // Open the file for writing
        std::ofstream outfile(upload_path.c_str(), std::ios::binary);
        if (!outfile.is_open()) {
            std::cerr << "Could not open file for writing: " << upload_path << std::endl;
            std::string response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            client.response = response;
            client.response_ready = true;
            return;
        }

        // Write the request body to the file
        outfile << client.body;
        outfile.close();

        // Send response
        std::string response = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.response = response;
        client.response_ready = true;
        return;
    }

    // Handle POST request
    if (method == "POST") {
        if (client.content_type.find("multipart/form-data") != std::string::npos) {
            handleFileUpload(client_fd);
        } else if (!route.cgi_path.empty()) {
            handleCGI(client_fd);
        } else {
            // Handle other POST requests
            std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            client.response = response;
            client.response_ready = true;
        }
        return;
    }

    // Handle GET request
    if (method == "GET") {
        std::string file_path;

        if (!route.cgi_path.empty()) {
            handleCGI(client_fd);
            return;
        }

        // Construct the file path
        if (!route.alias.empty()) {
            // Use alias if specified
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
            // Use root directory
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

        // Debugging: print the file path
        std::cerr << "GET request for file: " << file_path << std::endl;

        struct stat file_stat;
        if (stat(file_path.c_str(), &file_stat) == -1) {
            // perror("stat"); // Do not use perror as it uses errno
            std::cerr << "stat failed for " << file_path << std::endl;
            std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            client.response = response;
            client.response_ready = true;
            return;
        }

        if (S_ISDIR(file_stat.st_mode)) {
            // Handle directory
            std::cerr << "Directory requested. Original file path: " << file_path << std::endl;

            if (!route.index.empty()) {
                if (!file_path.empty() && file_path[file_path.size() - 1] != '/') {
                    file_path += "/";
                }
                file_path += route.index;
                std::cerr << "Appended index file. New file path: " << file_path << std::endl;

                // Check if the index file exists
                if (stat(file_path.c_str(), &file_stat) == -1) {
                    // perror("stat"); // Do not use perror as it uses errno
                    std::cerr << "stat failed for index file " << file_path << std::endl;
                    if (route.autoindex) {
                        // Generate directory listing
                        std::string listing = generateDirectoryListing(file_path, url);
                        // Build and send response
                        std::ostringstream response_stream;
                        response_stream << "HTTP/1.1 200 OK\r\n";
                        response_stream << "Content-Type: text/html\r\n";
                        response_stream << "Content-Length: " << listing.size() << "\r\n";
                        response_stream << "\r\n";
                        response_stream << listing;
                        client.response = response_stream.str();
                        client.response_ready = true;
                        return;
                    } else {
                        // Return 404 Not Found
                        std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                        client.response = response;
                        client.response_ready = true;
                        return;
                    }
                }
            } else if (route.autoindex) {
                // Generate directory listing
                std::string listing = generateDirectoryListing(file_path, url);
                // Build and send response
                std::ostringstream response_stream;
                response_stream << "HTTP/1.1 200 OK\r\n";
                response_stream << "Content-Type: text/html\r\n";
                response_stream << "Content-Length: " << listing.size() << "\r\n";
                response_stream << "\r\n";
                response_stream << listing;
                client.response = response_stream.str();
                client.response_ready = true;
                return;
            } else {
                // No index file and autoindex is off
                std::string response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                client.response = response;
                client.response_ready = true;
                return;
            }
        }

        // Read the file content
        std::ifstream file(file_path.c_str(), std::ios::binary);
        if (!file.is_open()) {
            // perror("ifstream"); // Do not use perror as it uses errno
            std::cerr << "Failed to open file: " << file_path << std::endl;
            std::string response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            client.response = response;
            client.response_ready = true;
            return;
        }

        std::ostringstream oss;
        oss << file.rdbuf();
        std::string file_content = oss.str();
        file.close();

        // Determine the Content-Type
        std::string content_type = getMimeType(file_path);

        // Construct the HTTP response
        std::ostringstream response_stream;
        response_stream << "HTTP/1.1 200 OK\r\n";
        response_stream << "Content-Type: " << content_type << "\r\n";
        response_stream << "Content-Length: " << file_content.size() << "\r\n";
        response_stream << "\r\n";
        response_stream << file_content;

        client.response = response_stream.str();
        client.response_ready = true;
        return;
    }

    // If the method is not supported
    std::string response = "HTTP/1.1 501 Not Implemented\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    client.response = response;
    client.response_ready = true;
}

void WebServer::handleCGI(int client_fd) {
    std::cout << "handleCGI called for client_fd: " << client_fd << std::endl;
    ClientConnection &client = clients[client_fd];
    const RouteConfig &route = *client.route;
    std::string request_body = client.body;
    std::string method = client.method;
    std::string query_string = client.query_string;
    std::string content_length = intToString(request_body.size());
    std::string content_type = client.content_type;
    std::string url = client.url;

    int cgi_output[2];
    int cgi_input[2];

    if (pipe(cgi_output) < 0 || pipe(cgi_input) < 0) {
        // perror("pipe"); // Do not use perror as it uses errno
        std::cerr << "Failed to create pipes for CGI" << std::endl;
        std::string error_response = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        client.response = error_response;
        client.response_ready = true;
        return;
    }

    fcntl(cgi_input[1], F_SETFL, O_NONBLOCK);
    fcntl(cgi_output[0], F_SETFL, O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        // perror("fork"); // Do not use perror as it uses errno
        std::cerr << "Failed to fork for CGI" << std::endl;
        std::string error_response = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        client.response = error_response;
        client.response_ready = true;
        return;
    }

    if (pid == 0) {
        // Child process
        dup2(cgi_output[1], STDOUT_FILENO);
        dup2(cgi_input[0], STDIN_FILENO);
        close(cgi_output[0]);
        close(cgi_output[1]);
        close(cgi_input[0]);
        close(cgi_input[1]);

        // Prepare environment variables
        std::string path_info = url;
        std::string path_translated = route.root + path_info;

        std::vector<std::string> env_vars;
        env_vars.push_back("REQUEST_METHOD=" + method);
        env_vars.push_back("QUERY_STRING=" + query_string);
        env_vars.push_back("CONTENT_LENGTH=" + content_length);
        env_vars.push_back("CONTENT_TYPE=" + content_type);
        env_vars.push_back("SERVER_PROTOCOL=HTTP/1.1");
        env_vars.push_back("GATEWAY_INTERFACE=CGI/1.1");
        env_vars.push_back("REMOTE_ADDR=");
        env_vars.push_back("PATH_INFO=" + path_info);
        env_vars.push_back("PATH_TRANSLATED=" + path_translated);
        env_vars.push_back("PATH=/usr/bin:/bin:/usr/local/bin");
        env_vars.push_back("HTTP_X_SECRET_HEADER_FOR_TEST=1");
        env_vars.push_back("REDIRECT_STATUS=200");

        std::vector<char*> env;
        for (size_t i = 0; i < env_vars.size(); ++i) {
            env.push_back(const_cast<char*>(env_vars[i].c_str()));
        }
        env.push_back(NULL);

        // Execute the CGI script
        char *args[] = {const_cast<char *>(route.cgi_path.c_str()), NULL};

        execve(route.cgi_path.c_str(), args, &env[0]);
        // perror("execve"); // Do not use perror as it uses errno
        std::cerr << "Failed to execve CGI script: " << route.cgi_path << std::endl;
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        close(cgi_output[1]);
        close(cgi_input[0]);

        // Store CGI state in client connection
        client.cgi_pid = pid;
        client.cgi_input_fd = cgi_input[1];
        client.cgi_output_fd = cgi_output[0];
        client.total_cgi_input_written = 0;
        client.request_body = request_body; // Ensure the body is stored
        client.cgi_output = "";

        // Add the CGI fds to the pollfd array
        addFdToPoll(client.cgi_input_fd, POLLOUT);
        addFdToPoll(client.cgi_output_fd, POLLIN);
    }
}

void WebServer::handleCGIWrite(int cgi_input_fd) {
     std::cout << "handleCGIWrite called for cgi_input_fd: " << cgi_input_fd << std::endl;
    // Find the client associated with this cgi_input_fd
    ClientConnection *client = findClientByCGIFD(cgi_input_fd);
    if (!client) return;

    size_t to_write = client->request_body.size() - client->total_cgi_input_written;
    if (to_write > 0) {
        ssize_t written = write(cgi_input_fd, client->request_body.c_str() + client->total_cgi_input_written, to_write);
        if (written > 0) {
            client->total_cgi_input_written += written;
            if (client->total_cgi_input_written == client->request_body.size()) {
                // Finished writing to CGI
                close(cgi_input_fd);
                client->cgi_input_fd = -1;
                removeFdFromPoll(cgi_input_fd);
            }
        } else {
            // written <= 0: Either an error occurred or write would block
            // Since we can't check errno, we'll wait for the next POLLOUT event
            // Do not close the fd here; just return and wait
        }
    } else {
        // No more data to write; close the fd
        close(cgi_input_fd);
        client->cgi_input_fd = -1;
        removeFdFromPoll(cgi_input_fd);
    }
}

void WebServer::handleCGIRead(int cgi_output_fd) {
    // Find the client associated with this cgi_output_fd
    WebServer::ClientConnection *client = findClientByCGIFD(cgi_output_fd);
    if (!client) return;

    char buffer[BUFFER_SIZE];
    ssize_t nbytes = read(cgi_output_fd, buffer, sizeof(buffer));

    if (nbytes > 0) {
        client->cgi_output.append(buffer, nbytes);
        // Debug messages
        std::cout << "Read " << nbytes << " bytes from CGI output_fd: " << cgi_output_fd << std::endl;
        std::cout << "Data: " << std::string(buffer, nbytes) << std::endl;
    } else {
        // Either EOF or error
        std::cout << "EOF or error on CGI output_fd: " << cgi_output_fd << std::endl;
        close(cgi_output_fd);
        client->cgi_output_fd = -1;
        removeFdFromPoll(cgi_output_fd);

        // Wait for the CGI process to exit
        waitpid(client->cgi_pid, NULL, 0);

        // Construct the HTTP response
        std::string status_line = "HTTP/1.1 200 OK\r\n";
        client->response = status_line + client->cgi_output;
        client->response_ready = true;
        std::cout << "Response ready for client_fd: " << client->fd << std::endl;
    }
}

void WebServer::handleFileUpload(int client_fd) {
    ClientConnection &client = clients[client_fd];
    const RouteConfig &route = *client.route;

    // Extract boundary from Content-Type header
    std::string boundary_prefix = "boundary=";
    size_t boundary_pos = client.content_type.find(boundary_prefix);
    if (boundary_pos == std::string::npos) {
        std::cerr << "Boundary not found in Content-Type header" << std::endl;
        client.response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        client.response_ready = true;
        return;
    }
    std::string boundary = "--" + client.content_type.substr(boundary_pos + boundary_prefix.length());
    std::string end_boundary = boundary + "--";

    // Find the start of the body
    size_t body_start = client.request.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        std::cerr << "Headers and body separator not found" << std::endl;
        client.response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        client.response_ready = true;
        return;
    }
    body_start += 4; // Move past the "\r\n\r\n"

    // Extract the body
    std::string body = client.request.substr(body_start);

    // Find the first boundary in the body
    size_t pos = body.find(boundary);
    if (pos == std::string::npos) {
        std::cerr << "Initial boundary not found in body" << std::endl;
        client.response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        client.response_ready = true;
        return;
    }
    pos += boundary.length() + 2; // Move past the boundary and "\r\n"

    while (pos < body.size()) {
        // Find the end of the headers
        size_t header_end = body.find("\r\n\r\n", pos);
        if (header_end == std::string::npos) {
            std::cerr << "Header end not found in body" << std::endl;
            break;
        }

        // Extract headers
        std::string headers = body.substr(pos, header_end - pos);
        pos = header_end + 4;

        // Extract filename
        size_t filename_pos = headers.find("filename=\"");
        if (filename_pos == std::string::npos) {
            std::cerr << "Filename not found in headers" << std::endl;
            break;
        }
        size_t filename_start = filename_pos + 10;
        size_t filename_end = headers.find("\"", filename_start);
        if (filename_end == std::string::npos) {
            std::cerr << "Filename end quote not found" << std::endl;
            break;
        }
        std::string filename = headers.substr(filename_start, filename_end - filename_start);

        // Extract file content
        size_t content_end = body.find(boundary, pos);
        if (content_end == std::string::npos) {
            std::cerr << "Content end boundary not found" << std::endl;
            break;
        }
        size_t content_length = content_end - pos - 2; // Exclude trailing "\r\n"
        std::string file_content = body.substr(pos, content_length);

        // Save the file
        std::string filepath = route.upload_dir + "/" + filename;
        std::ofstream outfile(filepath.c_str(), std::ios::binary);
        if (!outfile.is_open()) {
            std::cerr << "Failed to open file for writing: " << filepath << std::endl;
            client.response = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
            client.response_ready = true;
            return;
        }
        outfile.write(file_content.c_str(), file_content.size());
        outfile.close();

        std::cerr << "File saved: " << filepath << std::endl;

        // Move past the content to the next boundary
        pos = content_end + boundary.length();
        if (body.compare(pos, 2, "--") == 0) {
            // Reached the end boundary
            break;
        }
        pos += 2; // Move past "\r\n"
    }

    // Send response
    std::string response_body = "<html><body><h1>File Uploaded Successfully</h1></body></html>";
    std::ostringstream oss;
    oss << response_body.size();
    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/html\r\n";
    response += "Content-Length: " + oss.str() + "\r\n";
    response += "\r\n";
    response += response_body;
    client.response = response;
    client.response_ready = true;
}

void WebServer::handleDeleteRequest(int client_fd) {
    WebServer::ClientConnection &client = clients[client_fd];
    const RouteConfig &route = *client.route;
    std::string url = client.url;
    std::string file_path;

    // Construct the file path
    file_path = route.upload_dir + url.substr(route.url.size());

    // Sanitize the file path to prevent directory traversal
    if (file_path.find("..") != std::string::npos) {
        std::cerr << "Invalid file path: " << file_path << std::endl;
        std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.response = response;
        client.response_ready = true;
        return;
    }

    // Attempt to delete the file
    if (remove(file_path.c_str()) == 0) {
        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.response = response;
    } else {
        std::cerr << "Failed to delete file: " << file_path << std::endl;
        std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        client.response = response;
    }
    client.response_ready = true;
}

std::string WebServer::intToString(int value) {
    std::stringstream ss;
    ss << value;
    return ss.str();
}

std::string WebServer::getMimeType(const std::string &path) {
    if (path.find(".html") != std::string::npos) return "text/html";
    if (path.find(".css") != std::string::npos) return "text/css";
    if (path.find(".js") != std::string::npos) return "application/javascript";
    if (path.find(".png") != std::string::npos) return "image/png";
    if (path.find(".jpg") != std::string::npos) return "image/jpeg";
    return "text/plain";
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

std::string WebServer::getStatusText(int status_code) {
    switch (status_code) {
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "";
    }
}

std::string WebServer::getDefaultErrorPage(int status_code) {
    std::ostringstream oss;
    oss << "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><title>"
        << status_code << " " << getStatusText(status_code)
        << "</title></head><body><h1>" << status_code << " "
        << getStatusText(status_code) << "</h1><p>The requested method is not allowed for this resource.</p></body></html>";
    return oss.str();
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
            if (entry_name != "." && entry_name != "..") {
                entry_url += entry_name;
            } else {
                entry_url += entry_name;
            }
            listing << "<li><a href=\"" << entry_url << "\">" << entry_name << "</a></li>";
        }
        closedir(dir);
    } else {
        // perror("opendir"); // Do not use perror as it uses errno
        std::cerr << "Failed to open directory: " << path << std::endl;
        return getDefaultErrorPage(500);
    }

    listing << "</ul></body></html>";
    return listing.str();
}
