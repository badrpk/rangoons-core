CXX := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread
LDFLAGS := -lsqlite3
INC := -Iinclude
SRC := src/main.cpp src/server.cpp src/database.cpp src/utils.cpp
BIN := bin/rangoons

all: $(BIN)

$(BIN): $(SRC)
	mkdir -p bin
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN) src/*.o
