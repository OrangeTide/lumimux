# src/libvt/module.mk -- VT state machine parser and terminal buffer

lu_vt_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_vt_SRCS = vt_parse.c vt_buf.c vt_cell.c vt_state.c vt_ops.c
lu_vt_LIBS = lu_core lu_utf8
lu_vt_EXPORTED_CPPFLAGS = -I$(lu_vt_DIR)
LIBRARIES += lu_vt

test_vt_DIR := $(lu_vt_DIR)
test_vt_SRCS = test_vt.c
test_vt_LIBS = lu_vt lu_utf8 lu_core
EXECUTABLES += test_vt

define test_vt_TESTCMD
$(test_vt_EXEC)
endef
TEST_TARGETS += test_vt
