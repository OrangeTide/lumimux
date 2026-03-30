# src/libiox/module.mk -- I/O multiplexing (poll, signals)

lu_iox_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_iox_SRCS = iox_loop.c iox_fd.c iox_signal.c iox_timer.c
lu_iox_LIBS = lu_core
lu_iox_EXPORTED_CPPFLAGS = -I$(lu_iox_DIR)
LIBRARIES += lu_iox

test_iox_DIR := $(lu_iox_DIR)
test_iox_SRCS = test_iox.c
test_iox_LIBS = lu_iox lu_core
EXECUTABLES += test_iox

define test_iox_TESTCMD
$(test_iox_EXEC)
endef
TEST_TARGETS += test_iox
