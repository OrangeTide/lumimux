# src/libcore/module.mk -- common utilities (allocation, logging, paths)

lu_core_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_core_SRCS = xmalloc.c str.c log.c path.c daemonize.c dbg.c lu_umask.c \
	runtime_dir.c
lu_core_EXPORTED_CPPFLAGS = -I$(lu_core_DIR)
LIBRARIES += lu_core

test_runtime_dir_DIR := $(lu_core_DIR)
test_runtime_dir_SRCS = test_runtime_dir.c
test_runtime_dir_LIBS = lu_core
EXECUTABLES += test_runtime_dir

define test_runtime_dir_TESTCMD
$(test_runtime_dir_EXEC)
endef
TEST_TARGETS += test_runtime_dir
