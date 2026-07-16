# src/libipc/module.mk -- Unix domain socket IPC and message framing

lu_ipc_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_ipc_SRCS = ipc.c ipc_msg.c ipc_transport.c lumi_msg.c ipc_attr.c proxy_msg.c
lu_ipc_LIBS = lu_core
lu_ipc_EXPORTED_CPPFLAGS = -I$(lu_ipc_DIR)
LIBRARIES += lu_ipc

test_ipc_DIR := $(lu_ipc_DIR)
test_ipc_SRCS = test_ipc.c
test_ipc_LIBS = lu_ipc lu_core
EXECUTABLES += test_ipc

define test_ipc_TESTCMD
$(test_ipc_EXEC)
endef
TEST_TARGETS += test_ipc

test_ipc_transport_DIR := $(lu_ipc_DIR)
test_ipc_transport_SRCS = test_ipc_transport.c
test_ipc_transport_LIBS = lu_ipc lu_core
EXECUTABLES += test_ipc_transport

define test_ipc_transport_TESTCMD
$(test_ipc_transport_EXEC)
endef
TEST_TARGETS += test_ipc_transport

.PHONY: gen-ipc-msg
gen-ipc-msg:
	cd $(lu_ipc_DIR) && ./gen.sh lumi.idl lumi_msg
