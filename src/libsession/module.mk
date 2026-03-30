# src/libsession/module.mk -- window lifecycle management (PTY + VT)

lu_session_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_session_SRCS = window.c
lu_session_LIBS = lu_core lu_pty lu_vt lu_utf8
lu_session_EXPORTED_CPPFLAGS = -I$(lu_session_DIR)
LIBRARIES += lu_session
