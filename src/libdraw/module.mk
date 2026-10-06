# src/libdraw/module.mk -- backend-neutral drawing surface

lu_draw_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_draw_SRCS = draw.c
lu_draw_LIBS = lu_vt lu_utf8 lu_termlib
lu_draw_EXPORTED_CPPFLAGS = -I$(lu_draw_DIR)
LIBRARIES += lu_draw

test_draw_DIR := $(lu_draw_DIR)
test_draw_SRCS = test_draw.c
test_draw_LIBS = lu_draw lu_vt lu_utf8 lu_termlib
EXECUTABLES += test_draw

define test_draw_TESTCMD
$(test_draw_EXEC)
endef
TEST_TARGETS += test_draw
