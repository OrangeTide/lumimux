# src/libtaskbar/module.mk -- taskbar rendering and template expansion

lu_taskbar_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_taskbar_SRCS = taskbar.c taskbar_func.c
lu_taskbar_CPPFLAGS = -I$(lu_cfg_DIR)
lu_taskbar_LIBS = lu_core lu_utf8
lu_taskbar_EXPORTED_CPPFLAGS = -I$(lu_taskbar_DIR)
LIBRARIES += lu_taskbar

test_taskbar_DIR := $(lu_taskbar_DIR)
test_taskbar_SRCS = test_taskbar.c
test_taskbar_LIBS = lu_taskbar lu_cfg lu_core
EXECUTABLES += test_taskbar

define test_taskbar_TESTCMD
$(test_taskbar_EXEC)
endef
TEST_TARGETS += test_taskbar
