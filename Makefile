# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -Wextra -Werror -std=c++98 -Iincludes -g

# Directories
SRCDIR = srcs
INCDIR = includes

# Source files
SRCFILES = WebServer.cpp ServerConfig.cpp main.cpp
SRCS = $(addprefix $(SRCDIR)/, $(SRCFILES))

# Object files
OBJS = $(SRCS:.cpp=.o)

# Executable name
NAME = webserv

# Default target
all: $(NAME)

# Link the object files to create the executable
$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Compile .cpp to .o
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean rule
clean:
	rm -f $(OBJS)

# Full clean rule
fclean: clean
	rm -f $(NAME)

# Rebuild rule
re: fclean all

# Phony targets
.PHONY: all clean fclean re