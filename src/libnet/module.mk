# src/libnet/module.mk -- netchan reliable-UDP transport + vendored Monocypher
#
# Extracted from the netchan-v2 demo (see PROVENANCE.md). This is the
# transport core for FUTURE.md 11C networked connections. It does not yet
# link into any lumi binary; step 4 wires it behind the ipc_transport seam.

lu_net_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_net_SRCS = netchan.c nc_udp.c nc_crypto.c keystore.c nc_auth.c \
	third_party/monocypher.c ipc_transport_netchan.c
lu_net_CPPFLAGS = -I$(lu_net_DIR)third_party
lu_net_EXPORTED_CPPFLAGS = -I$(lu_net_DIR)
lu_net_LIBS = lu_ipc
LIBRARIES += lu_net

test_netchan_DIR := $(lu_net_DIR)
test_netchan_SRCS = test_netchan.c
test_netchan_LIBS = lu_net
EXECUTABLES += test_netchan

define test_netchan_TESTCMD
$(test_netchan_EXEC)
endef
TEST_TARGETS += test_netchan

test_nc_auth_DIR := $(lu_net_DIR)
test_nc_auth_SRCS = test_nc_auth.c
test_nc_auth_CPPFLAGS = -I$(lu_net_DIR)third_party
test_nc_auth_LIBS = lu_net
EXECUTABLES += test_nc_auth

define test_nc_auth_TESTCMD
$(test_nc_auth_EXEC)
endef
TEST_TARGETS += test_nc_auth

test_ipc_transport_net_DIR := $(lu_net_DIR)
test_ipc_transport_net_SRCS = test_ipc_transport_net.c
test_ipc_transport_net_CPPFLAGS = -I$(lu_net_DIR)third_party
test_ipc_transport_net_LIBS = lu_net lu_ipc lu_core
EXECUTABLES += test_ipc_transport_net

define test_ipc_transport_net_TESTCMD
$(test_ipc_transport_net_EXEC)
endef
TEST_TARGETS += test_ipc_transport_net
