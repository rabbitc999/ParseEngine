# library/big_number/Makefile —— 大数库：任意精度大整数 + 十进制浮点（C++17，只依赖标准库）
#
#   make            编译静态库 libbig_number.a、演示程序 big_number-demo 与两个测试
#   make check      编译并跑单元测试（断言计数风格）
#   make clean      删掉产物
#
# 产物落在仓库根目录的 build/big_number/（与 library/ 下其它库一致）。

CXX      ?= g++
AR       ?= ar
ARFLAGS  := rcs
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
UTF8FLAGS := -finput-charset=UTF-8 -fexec-charset=UTF-8
BUILD_DIR := ../../build/big_number

LIB   := $(BUILD_DIR)/libbig_number.a
DEMO  := $(BUILD_DIR)/big_number-demo
TESTS := $(BUILD_DIR)/bigint-tests
FLOAT_TESTS := $(BUILD_DIR)/bigfloat-tests
OBJS  := $(BUILD_DIR)/bigint.o $(BUILD_DIR)/bigfloat.o

.PHONY: all lib demo tests float-tests check clean

all: $(LIB) $(DEMO) $(TESTS) $(FLOAT_TESTS)

lib: $(LIB)

demo: $(DEMO)

tests: $(TESTS) $(FLOAT_TESTS)

float-tests: $(FLOAT_TESTS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/bigint.o: bigint.cpp bigint.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -c $< -o $@

$(BUILD_DIR)/bigfloat.o: bigfloat.cpp bigfloat.hpp bigint.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. -c $< -o $@

$(LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(DEMO): main.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. main.cpp $(LIB) -o $@

$(TESTS): test_bigint.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_bigint.cpp $(LIB) -o $@

$(FLOAT_TESTS): test_bigfloat.cpp $(LIB) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(UTF8FLAGS) -I. test_bigfloat.cpp $(LIB) -o $@

check: $(TESTS) $(FLOAT_TESTS) $(DEMO)
	$(TESTS)
	$(FLOAT_TESTS)
	$(DEMO)

clean:
	rm -rf $(BUILD_DIR)
