# src/libtui/module.mk -- text UI widget library (pure cell-grid logic)

lu_tui_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_tui_SRCS = tui_pad.c tui_theme.c tui_box.c tui_menu.c tui_list.c tui_sep.c
lu_tui_LIBS = lu_vt lu_utf8
lu_tui_EXPORTED_CPPFLAGS = -I$(lu_tui_DIR)
LIBRARIES += lu_tui

test_tui_DIR := $(lu_tui_DIR)
test_tui_SRCS = test_tui.c
test_tui_LIBS = lu_tui lu_vt lu_utf8 lu_core
EXECUTABLES += test_tui

define test_tui_TESTCMD
$(test_tui_EXEC)
endef
TEST_TARGETS += test_tui
