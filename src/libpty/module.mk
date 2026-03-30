# src/libpty/module.mk -- pseudo-terminal allocation and management

lu_pty_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_pty_SRCS = pty.c
lu_pty_LIBS = lu_core
lu_pty_EXPORTED_CPPFLAGS = -I$(lu_pty_DIR)
LIBRARIES += lu_pty
