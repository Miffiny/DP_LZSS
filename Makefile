ifeq ($(OS),Windows_NT)
TARGET := lzss.exe
RM_CMD = del /Q /F $(subst /,\,$(TARGET) $(OBJS)) 2>NUL
else
TARGET := lzss
RM_CMD = rm -f $(TARGET) $(OBJS)
endif

CXX := g++

CXXFLAGS := -std=c++17 -Wall -Wextra -O2
CPPFLAGS := -I. -ILZSS

SRCS := $(wildcard *.cpp) $(wildcard LZSS/*.cpp)
OBJS := $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	-$(RM_CMD)

.PHONY: all clean
