# src/cmd/proxy/module.mk -- SSH multiplexing proxy and cross-user broker

lu_proxy_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_proxy_SRCS = proxy.c
lu_proxy_LIBS = lu_iox lu_ipc lu_sessdir lu_core
lu_proxy_EXPORTED_CPPFLAGS = -I$(lu_proxy_DIR)
LIBRARIES += lu_proxy

test_proxy_DIR := $(lu_proxy_DIR)
test_proxy_SRCS = test_proxy.c
test_proxy_LIBS = lu_proxy lu_iox lu_ipc lu_sessdir lu_core
EXECUTABLES += test_proxy

define test_proxy_TESTCMD
$(test_proxy_EXEC)
endef
TEST_TARGETS += test_proxy
