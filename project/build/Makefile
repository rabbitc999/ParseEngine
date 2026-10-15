# Makefile —— custom 运行时，由 Script-Engine 自动生成（C++17，__MODE_NAME__）
#
# 目标：
#   make        默认编译静态库 libparser.a 与演示程序 parser_demo
#   make lib    只编译静态库 libparser.a
#   make demo   只编译演示程序 parser_demo
#   make run    编译并运行演示程序（ARGS 会当 argv 传给 main；内建 demo main
#               从标准输入读一行，用 `make run < 输入.txt` 更顺手）
#   make clean  删除中间文件与产物
#
# 覆盖变量示例：
#   make CXX=clang++ CXXFLAGS="-std=c++17 -O0 -g -Wall -Wextra"

# 注意：语义值类型 ParserValue_T 由文法（%value_type / %union / %{{...}}%）给出，
#       内存一律由标准库管理，**不支持自定义分配器**。
# 注意：make 内置定义了 CXX/AR/ARFLAGS，用 ?= 不会生效，这里显式赋值；
# 命令行传参（make CXX=clang++ ...）优先级最高，仍可覆盖。
CXX      = g++
AR       = ar
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LDLIBS   = -lm
ARFLAGS  = rcs

LIB_SRCS  := pl0_parser.cpp
DEMO_SRCS := pl0_main.cpp
HDRS      := pl0_parser.hpp pl0_parser_user.h
LIB_OBJS  := $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(LIB_SRCS)))
DEMO_OBJS := $(patsubst %.cpp,%.o,$(DEMO_SRCS))

LIB    := libparser.a
TARGET := parser_demo

.PHONY: all lib demo run clean

all: $(LIB) $(TARGET)

lib: $(LIB)

demo: $(TARGET)

$(LIB): $(LIB_OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(TARGET): $(DEMO_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(DEMO_OBJS) $(LIB) $(LDLIBS)

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET) $(ARGS)

clean:
	rm -f $(LIB_OBJS) $(DEMO_OBJS) $(LIB) $(TARGET)
