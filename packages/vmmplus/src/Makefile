CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -I.

SRCS = ppmc_predictor.cpp ctw_predictor.cpp bctw_predictor.cpp dctw_predictor.cpp \
       pst_predictor.cpp lzms_predictor.cpp vmm_factory.cpp vmm_bridge.cpp
OBJS = $(SRCS:.cpp=.o)

# Shared library for Lush/R consumption
LIBNAME = libvmmplus.so

# Test binary
TEST_BIN = test_vmm

all: $(LIBNAME) $(TEST_BIN)

$(LIBNAME): $(OBJS)
	$(CXX) -shared -fPIC -o $@ $^

$(TEST_BIN): test_vmm.o $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -fPIC -c -o $@ $<

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f *.o $(LIBNAME) $(TEST_BIN)

.PHONY: all test clean
