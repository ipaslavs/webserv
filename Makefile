# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -Wextra -Werror -std=c++98 -Iincludes

# Directories
SRCDIR = srcs
INCDIR = includes

# Source files (without directory prefix)
SRCFILES = WebServer.cpp ServerConfig.cpp main.cpp
SRCS = $(addprefix $(SRCDIR)/, $(SRCFILES))

# Executable name
NAME = webserv

# Default target
all: $(NAME)

# Rule to build the executable
$(NAME): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Clean rule
clean:
	rm -f $(NAME)

# Full clean rule
fclean: clean

# Rebuild rule
re: fclean all

# Phony targets
.PHONY: all clean fclean re
