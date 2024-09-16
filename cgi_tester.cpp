// cgi_tester.cpp
#include <iostream>
#include <cstdlib>

int main() {
    std::cout << "Content-Type: text/html\r\n\r\n";
    std::cout << "<html><head><title>CGI Tester</title></head><body>";
    std::cout << "<h1>CGI Script Output</h1>";
    std::cout << "<p>Environment Variables:</p><ul>";
    
    extern char **environ;
    for (char **env = environ; *env != 0; env++) {
        char *thisEnv = *env;
        std::cout << "<li>" << thisEnv << "</li>";
    }
    std::cout << "</ul>";
    std::cout << "</body></html>";
    return 0;
}
