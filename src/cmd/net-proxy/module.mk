# src/cmd/net-proxy/module.mk -- netchan network proxy (11C)

lu_netproxy_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_netproxy_SRCS = net_proxy.c
lu_netproxy_LIBS = lu_net lu_ipc lu_sessdir lu_core
lu_netproxy_EXPORTED_CPPFLAGS = -I$(lu_netproxy_DIR)
LIBRARIES += lu_netproxy

test_net_proxy_DIR := $(lu_netproxy_DIR)
test_net_proxy_SRCS = test_net_proxy.c
test_net_proxy_LIBS = lu_netproxy lu_net lu_ipc lu_sessdir lu_core
EXECUTABLES += test_net_proxy

define test_net_proxy_TESTCMD
$(test_net_proxy_EXEC)
endef
TEST_TARGETS += test_net_proxy
