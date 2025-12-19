CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread
BINARY := agartha_server
PORT ?= 80

all: $(BINARY)

$(BINARY): server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

start: $(BINARY)
	sudo -n env PORT=$(PORT) ./$(BINARY)

.PHONY: all start
