# src/libtui_draw/module.mk -- draw-surface backend for the libtui pad stack

lu_tui_draw_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_tui_draw_SRCS = tui_draw.c
lu_tui_draw_LIBS = lu_tui lu_draw lu_vt lu_utf8
lu_tui_draw_EXPORTED_CPPFLAGS = -I$(lu_tui_draw_DIR)
LIBRARIES += lu_tui_draw

test_tui_draw_DIR := $(lu_tui_draw_DIR)
test_tui_draw_SRCS = test_tui_draw.c
test_tui_draw_LIBS = lu_tui_draw lu_tui lu_draw lu_vt lu_utf8 lu_core
EXECUTABLES += test_tui_draw

define test_tui_draw_TESTCMD
$(test_tui_draw_EXEC)
endef
TEST_TARGETS += test_tui_draw
