# src/libattr/module.mk -- transactional attribute store

lu_attr_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_attr_SRCS = attr_store.c
lu_attr_EXPORTED_CPPFLAGS = -I$(lu_attr_DIR)
LIBRARIES += lu_attr

test_attr_store_DIR := $(lu_attr_DIR)
test_attr_store_SRCS = test_attr_store.c
test_attr_store_LIBS = lu_attr
EXECUTABLES += test_attr_store

define test_attr_store_TESTCMD
$(test_attr_store_EXEC)
endef
TEST_TARGETS += test_attr_store
