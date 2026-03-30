# src/libtxl/module.mk -- terminal translation engine (data-driven rules)

lu_txl_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_txl_SRCS = txl.c
lu_txl_LIBS = lu_termlib lu_core
lu_txl_EXPORTED_CPPFLAGS = -I$(lu_txl_DIR)
LIBRARIES += lu_txl

test_txl_DIR := $(lu_txl_DIR)
test_txl_SRCS = test_txl.c
test_txl_LIBS = lu_txl lu_termlib lu_core
EXECUTABLES += test_txl

define test_txl_TESTCMD
$(test_txl_EXEC)
endef
TEST_TARGETS += test_txl
