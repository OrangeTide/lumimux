# src/libdraw_term/module.mk -- terminal backend for the draw surface

lu_draw_term_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_draw_term_SRCS = draw_term.c
lu_draw_term_LIBS = lu_draw lu_render lu_txl lu_tio lu_termlib lu_vt lu_utf8 \
	lu_core
lu_draw_term_EXPORTED_CPPFLAGS = -I$(lu_draw_term_DIR)
LIBRARIES += lu_draw_term

test_draw_term_DIR := $(lu_draw_term_DIR)
test_draw_term_SRCS = test_draw_term.c
test_draw_term_LIBS = lu_draw_term lu_draw lu_render lu_txl lu_tio lu_termlib \
	lu_vt lu_utf8 lu_core
EXECUTABLES += test_draw_term

define test_draw_term_TESTCMD
$(test_draw_term_EXEC)
endef
TEST_TARGETS += test_draw_term
