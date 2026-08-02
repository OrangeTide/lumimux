# src/cmd/mserver/module.mk -- lumi-mserver single-PTY micro-server

lu_mserver_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_mserver_SRCS = mserver.c
lu_mserver_CPPFLAGS = -I$(lu_mserver_DIR)../..
lu_mserver_LIBS = lu_iox lu_ipc lu_attr lu_sessdir lu_session lu_vt \
	lu_utf8 lu_pty lu_core
lu_mserver_EXPORTED_CPPFLAGS = -I$(lu_mserver_DIR)
LIBRARIES += lu_mserver

test_mserver_DIR := $(lu_mserver_DIR)
test_mserver_SRCS = test_mserver.c
test_mserver_LIBS = lu_mserver lu_iox lu_ipc lu_attr lu_sessdir lu_session \
	lu_vt lu_utf8 lu_pty lu_core
test_mserver_LDLIBS = $(if $(findstring darwin,$(TARGET_TRIPLET)),,-lutil)
EXECUTABLES += test_mserver

define test_mserver_TESTCMD
$(test_mserver_EXEC)
endef
TEST_TARGETS += test_mserver
