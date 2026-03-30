# src/libkeys/module.mk -- key binding table and Screen-compatible defaults

lu_keys_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_keys_SRCS = keys.c
lu_keys_CPPFLAGS = -I$(lu_cfg_DIR)
lu_keys_LIBS = lu_core
lu_keys_EXPORTED_CPPFLAGS = -I$(lu_keys_DIR)
LIBRARIES += lu_keys

test_keys_DIR := $(lu_keys_DIR)
test_keys_SRCS = test_keys.c
test_keys_LIBS = lu_keys lu_cfg lu_core
EXECUTABLES += test_keys

define test_keys_TESTCMD
$(test_keys_EXEC)
endef
TEST_TARGETS += test_keys
