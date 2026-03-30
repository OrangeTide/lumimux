# src/librender/module.mk -- differential screen renderer

lu_render_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_render_SRCS = render.c
lu_render_LIBS = lu_core lu_vt lu_utf8 lu_tio lu_txl
lu_render_EXPORTED_CPPFLAGS = -I$(lu_render_DIR)
LIBRARIES += lu_render

test_render_DIR := $(lu_render_DIR)
test_render_SRCS = test_render.c
test_render_LIBS = lu_render lu_vt lu_utf8 lu_tio lu_txl lu_termlib lu_core
EXECUTABLES += test_render

define test_render_TESTCMD
$(test_render_EXEC)
endef
TEST_TARGETS += test_render
