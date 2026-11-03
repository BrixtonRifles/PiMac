# Host (macOS/Linux) build of the portable core, for unit tests and previews.

CXX      ?= c++
CXXFLAGS := -std=c++14 -O1 -g -Wall -Wextra -DPIMAC_HOST -I../src

CORE_SRC := $(wildcard ../src/core/*.cpp)
CORE_HDR := $(wildcard ../src/core/*.h)

.PHONY: all run preview clean

all: run

test_core: test_core.cpp $(CORE_SRC) $(CORE_HDR)
	$(CXX) $(CXXFLAGS) -o $@ test_core.cpp $(CORE_SRC)

preview: preview.cpp $(CORE_SRC) $(CORE_HDR)
	$(CXX) $(CXXFLAGS) -o $@ preview.cpp $(CORE_SRC)

run: test_core
	./test_core

previews: preview
	./preview screen.bmp
	./preview screen-menu.bmp menu

clean:
	rm -rf test_core preview *.bmp *.png *.dSYM
