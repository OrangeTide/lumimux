# src/libcfg/module.mk -- configuration file parser (.lumirc)

lu_cfg_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_cfg_SRCS = cfg.c
lu_cfg_LIBS = lu_core
lu_cfg_EXPORTED_CPPFLAGS = -I$(lu_cfg_DIR)
LIBRARIES += lu_cfg

test_cfg_DIR := $(lu_cfg_DIR)
test_cfg_SRCS = test_cfg.c
test_cfg_LIBS = lu_cfg lu_core
EXECUTABLES += test_cfg

define test_cfg_TESTCMD
$(test_cfg_EXEC)
endef
TEST_TARGETS += test_cfg
