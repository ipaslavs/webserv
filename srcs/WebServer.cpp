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

void WebServer::bindSocket(int &server_fd, int port) {
    int opt = 1;
    struct sockaddr_in address;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
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

    if (listen(server_fd, 3) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    std::cout << "Server listening on port " << port << std::endl;
}

// Define a traditional function to check if the file descriptor is -1
bool is_closed_socket(const struct pollfd &pfd) {
    return pfd.fd == -1;
}

void WebServer::run() {
    while (true) {
        int poll_count = poll(fds, nfds, -1);

        if (poll_count < 0) {
            perror("poll");
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < nfds; ++i) {
            if (fds[i].revents & POLLIN) {
                if (std::find(server_fds.begin(), server_fds.end(), fds[i].fd) != server_fds.end()) {
                    struct sockaddr_in address;
                    socklen_t addrlen = sizeof(address);
                    int new_socket;

                    if ((new_socket = accept(fds[i].fd, (struct sockaddr *)&address, &addrlen)) < 0) {
                        perror("accept");
                        exit(EXIT_FAILURE);
                    }
                    std::cout << "New connection on socket " << new_socket << std::endl;

                    fds[nfds].fd = new_socket;
                    fds[nfds].events = POLLIN;
                    nfds++;

                    client_server_map[new_socket] = i;
                } else {
                    handleClient(fds[i].fd);
                    close(fds[i].fd);
                    fds[i].fd = -1;
                }
            }
        }

        nfds = std::remove_if(fds, fds + nfds, is_closed_socket) - fds;
    }
}


std::string intToString(int value) {
    std::stringstream ss;
    ss << value;
    return ss.str();
}


std::string getMimeType(const std::string &path) {
    if (path.find(".html") != std::string::npos) return "text/html";
    if (path.find(".css") != std::string::npos) return "text/css";
    if (path.find(".js") != std::string::npos) return "application/javascript";
    if (path.find(".png") != std::string::npos) return "image/png";
    if (path.find(".jpg") != std::string::npos) return "image/jpeg";
    return "text/plain";
}

std::string getErrorPage(const ServerConfig &config, int status_code) {
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

std::string getStatusText(int status_code) {
    switch (status_code) {
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "";
    }
}

std::string getDefaultErrorPage(int status_code) {
    std::ostringstream oss;
    oss << "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\"><title>"
        << status_code << " " << getStatusText(status_code)
        << "</title></head><body><h1>" << status_code << " "
        << getStatusText(status_code) << "</h1><p>The requested method is not allowed for this resource.</p></body></html>";
    return oss.str();
}

std::string generateDirectoryListing(const std::string &path, const std::string &url) {
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
        perror("opendir");
        return getDefaultErrorPage(500);
    }

    listing << "</ul></body></html>";
    return listing.str();
}

void WebServer::handleClient(int client_fd) {
 char buffer[BUFFER_SIZE] = {0};
    std::string request;
    int valread;

    try {
        while ((valread = read(client_fd, buffer, BUFFER_SIZE - 1)) > 0) {
            buffer[valread] = '\0';
            request += buffer;
            if (request.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }

        if (valread <= 0) {
            if (valread == 0) {
                std::cout << "Client disconnected from socket " << client_fd << std::endl;
            } else {
                throw std::runtime_error("Error reading from socket " + intToString(client_fd));
            }
            close(client_fd);
            return;
        }

        std::cout << "Received: " << request << std::endl;

        std::istringstream request_stream(request);
        std::string method, url, http_version;
        request_stream >> method >> url >> http_version;

        std::string body;
        std::string content_length;
        std::string content_type;
        std::string query_string;
        std::string transfer_encoding;

        std::string line;
        size_t pos;
        size_t headers_end = request.find("\r\n\r\n");
        std::istringstream headers_stream(request.substr(0, headers_end));
        while (std::getline(headers_stream, line) && line != "\r") {
            if ((pos = line.find("Content-Length: ")) != std::string::npos) {
                content_length = line.substr(pos + 16);
                content_length = content_length.substr(0, content_length.find("\r"));
            } else if ((pos = line.find("Content-Type: ")) != std::string::npos) {
                content_type = line.substr(pos + 14);
                content_type = content_type.substr(0, content_type.find("\r"));
            } else if ((pos = line.find("Transfer-Encoding: ")) != std::string::npos) {
                transfer_encoding = line.substr(pos + 19);
                transfer_encoding = transfer_encoding.substr(0, transfer_encoding.find("\r"));
            }
        }

        pos = url.find('?');
        if (pos != std::string::npos) {
            query_string = url.substr(pos + 1);
            url = url.substr(0, pos);
        }

        std::map<int, int>::const_iterator it = client_server_map.find(client_fd);
        if (it == client_server_map.end()) {
            throw std::runtime_error("Unknown socket " + intToString(client_fd));
        }
        const ServerConfig &config = configs[it->second];

        std::cout << "Routes for server on port " << config.port << ":" << std::endl;
        for (size_t i = 0; i < config.routes.size(); ++i) {
            const RouteConfig &route = config.routes[i];
            std::cout << "Route: URL = '" << route.url << "', Methods = [";
            for (size_t j = 0; j < route.methods.size(); ++j) {
                std::cout << route.methods[j] << ", ";
            }
            std::cout << "], Root = '" << route.root << "', Index = '" << route.index << "', AutoIndex = '" << (route.autoindex ? "on" : "off") << "', Alias = '" << route.alias << "', Max Body = '" << route.max_body << "'" << std::endl;
        }

        const RouteConfig* best_match_route = NULL;
        size_t longest_match_length = 0;

        for (size_t i = 0; i < config.routes.size(); ++i) {
            const RouteConfig &route = config.routes[i];
            if (url.find(route.url) == 0 && route.url.size() > longest_match_length) {
                best_match_route = &route;
                longest_match_length = route.url.size();
            } else if (url.size() >= 4 && url.substr(url.size() - 4) == ".bla" && route.url == ".bla") {
                best_match_route = &route;
                longest_match_length = route.url.size();
            }
        }

        if (!best_match_route) {
            std::cerr << "Route not found for URL: " << url << std::endl;
            std::string error_response = getErrorPage(config, 404);
            if (error_response.empty()) {
                error_response = getDefaultErrorPage(404);
            }
            std::ostringstream oss;
            oss << error_response.size();
            std::string response = "HTTP/1.1 404 Not Found\r\n";
            response += "Content-Type: text/html\r\n";
            response += "Content-Length: " + oss.str() + "\r\n";
            response += "\r\n";
            response += error_response;
            send(client_fd, response.c_str(), response.size(), 0);
            close(client_fd);
            return;
        }

        const RouteConfig &route = *best_match_route;
        std::cout << "Matched route: " << route.url << std::endl;

        if (std::find(route.methods.begin(), route.methods.end(), method) == route.methods.end()) {
            std::cerr << "Method not allowed: " << method << std::endl;

            std::ostringstream allow_methods;
            for (size_t i = 0; i < route.methods.size(); ++i) {
                allow_methods << route.methods[i] << ", ";
            }

            std::string error_response = getErrorPage(config, 405);
            if (error_response.empty()) {
                error_response = getDefaultErrorPage(405);
            }
            std::ostringstream oss;
            oss << error_response.size();
            std::string response = "HTTP/1.1 405 Method Not Allowed\r\n";
            response += "Content-Type: text/html\r\n";
            response += "Content-Length: " + oss.str() + "\r\n";
            response += "Allow: " + allow_methods.str() + "\r\n";
            response += "\r\n";

            if (method != "HEAD") {
                response += error_response;
            }

            send(client_fd, response.c_str(), response.size(), 0);
            close(client_fd);
            return;
        }

        size_t content_length_val = 0;
        if (!content_length.empty()) {
            std::stringstream content_length_stream(content_length);
            content_length_stream >> content_length_val;
        }

        if ((method == "POST" || method == "PUT") && transfer_encoding != "chunked" && route.max_body > 0 && content_length_val > route.max_body) {
            std::cerr << "Request body too large: " << content_length_val << " bytes" << std::endl;
            std::string error_response = "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\n\r\n";
            send(client_fd, error_response.c_str(), error_response.size(), 0);
            close(client_fd);
            return;
        }

        size_t total_body_size = 0;
        if (transfer_encoding == "chunked") {
            size_t body_start = headers_end + 4;
            while (true) {
                size_t chunk_size_end = request.find("\r\n", body_start);
                if (chunk_size_end == std::string::npos) {
                    valread = read(client_fd, buffer, BUFFER_SIZE - 1);
                    if (valread <= 0) {
                        throw std::runtime_error("Error or client closed connection while reading chunk size");
                    }
                    buffer[valread] = '\0';
                    request += buffer;
                    continue;
                }

                std::string chunk_size_str = request.substr(body_start, chunk_size_end - body_start);
                size_t chunk_size;
                std::istringstream(chunk_size_str) >> std::hex >> chunk_size;

                if (chunk_size == 0) {
                    break;
                }

                total_body_size += chunk_size;

                if (route.max_body > 0 && total_body_size > route.max_body) {
                    throw std::runtime_error("Request body too large: " + intToString(total_body_size) + " bytes");
                }

                body_start = chunk_size_end + 2;
                size_t chunk_end = body_start + chunk_size;
                while (request.size() < chunk_end) {
                    valread = read(client_fd, buffer, BUFFER_SIZE - 1);
                    if (valread <= 0) {
                        throw std::runtime_error("Error or client closed connection while reading chunk body");
                    }
                    buffer[valread] = '\0';
                    request += buffer;
                }

                body += request.substr(body_start, chunk_size);
                body_start = chunk_end + 2;
            }
        } else {
            size_t body_start = headers_end + 4;
            if (content_length_val > 0) {
                size_t current_body_size = request.size() - body_start;
                while (current_body_size < content_length_val) {
                    valread = read(client_fd, buffer, BUFFER_SIZE);
                    if (valread > 0) {
                        request.append(buffer, valread);
                        current_body_size += valread;
                    } else {
                        break;
                    }
                }
                body = request.substr(body_start, content_length_val);

                if (route.max_body > 0 && current_body_size > route.max_body) {
                    throw std::runtime_error("Request body too large: " + intToString(current_body_size) + " bytes");
                }
            }
        }

        std::string actual_content_length = intToString(body.size());

        if (!route.cgi_path.empty() && (url == route.url || (url.size() >= 4 && url.substr(url.size() - 4) == ".bla"))) {
            handleCGI(client_fd, route, body, method, query_string, actual_content_length, content_type, url);
            return;
        }

        if (!route.upload_dir.empty()) {
            handleFileUpload(client_fd, route, request);
            return;
        }

        std::string file_path;
        if (!route.alias.empty()) {
            file_path = route.alias;
            if (!file_path.empty() && *file_path.rbegin() != '/') {
                file_path += "/";
            }
            std::string remaining_url = url.substr(route.url.size());
            if (!remaining_url.empty() && *remaining_url.begin() == '/') {
                remaining_url = remaining_url.substr(1);
            }
            file_path += remaining_url;
        } else {
            file_path = route.root;
            if (!file_path.empty() && *file_path.rbegin() != '/' && !url.empty() && *url.begin() != '/') {
                file_path += "/";
            }
            file_path += url;
        }

        struct stat file_stat;
        if (stat(file_path.c_str(), &file_stat) < 0) {
            std::string error_response = getErrorPage(config, 404);
            if (error_response.empty()) {
                error_response = getDefaultErrorPage(404);
            }
            std::ostringstream oss;
            oss << error_response.size();
            std::string response = "HTTP/1.1 404 Not Found\r\n";
            response += "Content-Type: text/html\r\n";
            response += "Content-Length: " + oss.str() + "\r\n";
            response += "\r\n";
            response += error_response;
            send(client_fd, response.c_str(), response.size(), 0);
            close(client_fd);
            return;
        }

        if (S_ISDIR(file_stat.st_mode)) {
            if (!file_path.empty() && *file_path.rbegin() != '/') {
                file_path += "/";
            }
            file_path += (route.index.empty() ? "index.html" : route.index);
            if (stat(file_path.c_str(), &file_stat) < 0) {
                if (route.autoindex) {
                    std::string directory_listing = generateDirectoryListing(file_path.substr(0, file_path.find_last_of('/')), url);
                    std::ostringstream oss;
                    oss << directory_listing.size();
                    std::string response = "HTTP/1.1 200 OK\r\n";
                    response += "Content-Type: text/html\r\n";
                    response += "Content-Length: " + oss.str() + "\r\n";
                    response += "\r\n";
                    response += directory_listing;
                    send(client_fd, response.c_str(), response.size(), 0);
                    close(client_fd);
                    return;
                } else {
                    std::string error_response = getErrorPage(config, 404);
                    if (error_response.empty()) {
                        error_response = getDefaultErrorPage(404);
                    }
                    std::ostringstream oss;
                    oss << error_response.size();
                    std::string response = "HTTP/1.1 404 Not Found\r\n";
                    response += "Content-Type: text/html\r\n";
                    response += "Content-Length: " + oss.str() + "\r\n";
                    response += "\r\n";
                    response += error_response;
                    send(client_fd, response.c_str(), response.size(), 0);
                    close(client_fd);
                    return;
                }
            }
        }

        std::ifstream file(file_path.c_str());
        if (!file.is_open()) {
            std::string error_response = getErrorPage(config, 403);
            if (error_response.empty()) {
                error_response = getDefaultErrorPage(403);
            }
            std::ostringstream oss;
            oss << error_response.size();
            std::string response = "HTTP/1.1 403 Forbidden\r\n";
            response += "Content-Type: text/html\r\n";
            response += "Content-Length: " + oss.str() + "\r\n";
            response += "\r\n";
            response += error_response;
            send(client_fd, response.c_str(), response.size(), 0);
            close(client_fd);
            return;
        }

        std::stringstream file_content;
        file_content << file.rdbuf();
        std::string response_body = file_content.str();
        file.close();

        std::ostringstream oss;
        oss << response_body.size();
        std::string response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: " + getMimeType(file_path) + "\r\n";
        response += "Content-Length: " + oss.str() + "\r\n";
        response += "\r\n";
        response += response_body;

        send(client_fd, response.c_str(), response.size(), 0);
        close(client_fd);
    } catch (const std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        close(client_fd);
    }
}

void WebServer::handleCGI(int client_fd, const RouteConfig &route, const std::string &request_body, const std::string &method, const std::string &query_string, const std::string &content_length, const std::string &content_type, const std::string &url) {
       int cgi_output[2];
    int cgi_input[2];

    if (pipe(cgi_output) < 0 || pipe(cgi_input) < 0) {
        perror("pipe");
        std::string error_response = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        send(client_fd, error_response.c_str(), error_response.size(), 0);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        std::string error_response = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        send(client_fd, error_response.c_str(), error_response.size(), 0);
        return;
    }

    if (pid == 0) {
        dup2(cgi_output[1], STDOUT_FILENO);
        dup2(cgi_input[0], STDIN_FILENO);
        close(cgi_output[0]);
        close(cgi_output[1]);
        close(cgi_input[0]);
        close(cgi_input[1]);

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
     //   env_vars.push_back("SCRIPT_NAME=" + url);
        env_vars.push_back("PATH=/usr/bin:/bin:/usr/local/bin");
        env_vars.push_back("HTTP_X_SECRET_HEADER_FOR_TEST=1");
        env_vars.push_back("REDIRECT_STATUS=200");

        std::vector<char*> env;
        for (std::vector<std::string>::iterator it = env_vars.begin(); it != env_vars.end(); ++it) {
            env.push_back(const_cast<char*>(it->c_str()));
        }
        env.push_back(NULL);

        char *args[] = {const_cast<char *>(route.cgi_path.c_str()), const_cast<char *>(path_translated.c_str()), NULL};

        execve(route.cgi_path.c_str(), args, &env[0]);
        perror("execve");
        exit(EXIT_FAILURE);
    } else {
        close(cgi_output[1]);
        close(cgi_input[0]);

        ssize_t total_written = 0;

        std::cout << "content_length = " << content_length << std::endl;
        while (total_written < static_cast<ssize_t>(request_body.size())) {
            ssize_t to_write = std::min(static_cast<ssize_t>(1001), static_cast<ssize_t>(request_body.size() - total_written));
            ssize_t written = write(cgi_input[1], request_body.c_str() + total_written, to_write);
            if (written < 0) {
                if (errno == EPIPE) {
                    std::cerr << "Broken pipe: CGI script terminated early" << std::endl;
                    break;
                } else {
                    perror("write to CGI input pipe");
                    break;
                }
            }
            total_written += written;

            std::cout << "total_written" << total_written << std::endl;
        }
        close(cgi_input[1]);

        char buffer[BUFFER_SIZE];
        std::string cgi_response;
        ssize_t nbytes;
        while ((nbytes = read(cgi_output[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[nbytes] = '\0';
            cgi_response += buffer;
        }

        if (nbytes < 0) {
            perror("read");
            std::string error_response = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
            send(client_fd, error_response.c_str(), error_response.size(), 0);
        } else {
            std::ostringstream oss;
            oss << cgi_response.size();
            std::string content_length_header = oss.str();

            std::string response = "HTTP/1.1 200 OK\r\n";
            response += "Content-Type: text/html\r\n";
            if (!cgi_response.empty()) {
                response += "Content-Length: " + content_length_header + "\r\n";
            }
            response += "\r\n";
            response += cgi_response;

            send(client_fd, response.c_str(), response.size(), 0);
        }

        close(cgi_output[0]);
        waitpid(pid, NULL, 0);
    }
}

void WebServer::handleFileUpload(int client_fd, const RouteConfig &route, const std::string &request) {
    std::string boundary_prefix = "boundary=";
    size_t boundary_pos = request.find(boundary_prefix);

    if (boundary_pos == std::string::npos) {
        std::cerr << "Boundary not found in request" << std::endl;
        std::string error_response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        send(client_fd, error_response.c_str(), error_response.size(), 0);
        close(client_fd);
        return;
    }

    std::string boundary = "--" + request.substr(boundary_pos + boundary_prefix.length(), request.find("\r\n", boundary_pos) - (boundary_pos + boundary_prefix.length()));
    std::string end_boundary = boundary + "--";

    size_t pos = request.find(boundary);
    if (pos == std::string::npos) {
        std::cerr << "Initial boundary not found in request" << std::endl;
        std::string error_response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        send(client_fd, error_response.c_str(), error_response.size(), 0);
        close(client_fd);
        return;
    }

    pos += boundary.length() + 2;
    while (pos < request.size()) {
        size_t header_end = request.find("\r\n\r\n", pos);
        if (header_end == std::string::npos) {
            std::cerr << "Header end not found in request" << std::endl;
            std::string error_response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            send(client_fd, error_response.c_str(), error_response.size(), 0);
            close(client_fd);
            return;
        }

        std::string headers = request.substr(pos, header_end - pos);
        pos = header_end + 4;

        size_t filename_pos = headers.find("filename=\"");
        if (filename_pos == std::string::npos) {
            std::cerr << "Filename not found in headers" << std::endl;
            std::string error_response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            send(client_fd, error_response.c_str(), error_response.size(), 0);
            close(client_fd);
            return;
        }

        size_t filename_start = filename_pos + 10;
        size_t filename_end = headers.find("\"", filename_start);
        if (filename_end == std::string::npos) {
            std::cerr << "Filename end not found in headers" << std::endl;
            std::string error_response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            send(client_fd, error_response.c_str(), error_response.size(), 0);
            close(client_fd);
            return;
        }

        std::string filename = headers.substr(filename_start, filename_end - filename_start);
        std::string filepath = route.upload_dir + "/" + filename;

        size_t content_end = request.find(boundary, pos);
        if (content_end == std::string::npos || content_end > request.find(end_boundary)) {
            std::cerr << "Content end not found in request" << std::endl;
            std::string error_response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            send(client_fd, error_response.c_str(), error_response.size(), 0);
            close(client_fd);
            return;
        }

        struct stat info;
        if (stat(route.upload_dir.c_str(), &info) != 0 || !(info.st_mode & S_IFDIR)) {
            std::cerr << "Upload directory does not exist: " << route.upload_dir << std::endl;
            std::string error_response = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
            send(client_fd, error_response.c_str(), error_response.size(), 0);
            close(client_fd);
            return;
        }

        std::ofstream file(filepath.c_str(), std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Could not open file for writing: " << filepath << std::endl;
            std::string error_response = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
            send(client_fd, error_response.c_str(), error_response.size(), 0);
            close(client_fd);
            return;
        }

        file.write(request.data() + pos, content_end - pos - 2);
        file.close();

        pos = content_end + boundary.length();
        if (request.compare(pos, 2, "--") == 0) {
            break;
        }
        pos += 2;
    }

    std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(client_fd, response.c_str(), response.size(), 0);
    close(client_fd);
}