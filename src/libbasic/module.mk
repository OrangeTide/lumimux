# src/libbasic/module.mk -- BASIC-style expression and program interpreter

lu_basic_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_basic_SRCS = basic.c
lu_basic_LDLIBS = -lm
lu_basic_EXPORTED_CPPFLAGS = -I$(lu_basic_DIR)
LIBRARIES += lu_basic

test_basic_DIR := $(lu_basic_DIR)
test_basic_SRCS = test_basic.c
test_basic_LIBS = lu_basic
test_basic_LDLIBS = -lm
EXECUTABLES += test_basic

define test_basic_TESTCMD
$(test_basic_EXEC)
endef
TEST_TARGETS += test_basic
