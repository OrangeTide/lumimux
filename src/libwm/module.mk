# src/libwm/module.mk -- overlapping window manager compositor

lu_wm_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_wm_SRCS = wm.c
lu_wm_LIBS = lu_vt lu_tui lu_utf8
lu_wm_EXPORTED_CPPFLAGS = -I$(lu_wm_DIR)
LIBRARIES += lu_wm

test_wm_DIR := $(lu_wm_DIR)
test_wm_SRCS = test_wm.c
test_wm_LIBS = lu_wm lu_vt lu_tui lu_utf8 lu_core
EXECUTABLES += test_wm

define test_wm_TESTCMD
$(test_wm_EXEC)
endef
TEST_TARGETS += test_wm
