ifeq ($(OS),Windows_NT)
TARGET := lzss.exe
RM_CMD = del /Q /F $(subst /,\,$(TARGET) $(OBJS)) 2>NUL
else
TARGET := lzss
RM_CMD = rm -f $(TARGET) $(OBJS)
endif

CXX := g++
CC := gcc

CXXFLAGS := -std=c++17 -Wall -Wextra -O2
CFLAGS := -std=c11 -Wall -Wextra -O2
CPPFLAGS := -I. -ILZSS -DLZSS_DEFAULT_ENTROPY_CODEC=LZSS_ENTROPY_CODEC_TANS

CPP_SRCS := $(wildcard *.cpp) $(wildcard LZSS/*.cpp)
C_SRCS := $(wildcard *.c)
OBJS := $(CPP_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	-$(RM_CMD)

.PHONY: all clean
