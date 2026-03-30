# src/libstatus/module.mk -- status line rendering and template expansion

lu_status_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_status_SRCS = status.c status_func.c
lu_status_CPPFLAGS = -I$(lu_cfg_DIR)
lu_status_LIBS = lu_core
lu_status_EXPORTED_CPPFLAGS = -I$(lu_status_DIR)
LIBRARIES += lu_status

test_status_DIR := $(lu_status_DIR)
test_status_SRCS = test_status.c
test_status_LIBS = lu_status lu_cfg lu_core
EXECUTABLES += test_status

define test_status_TESTCMD
$(test_status_EXEC)
endef
TEST_TARGETS += test_status
