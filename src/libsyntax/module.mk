# src/libsyntax/module.mk -- data-driven syntax highlighter

lu_syntax_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_syntax_SRCS = syntax.c syn_c.c syn_sh.c syn_lua.c syn_py.c syn_rust.c \
	syn_go.c syn_bas.c syn_fth.c syn_js.c syn_html.c syn_nasm.c syn_gas.c \
	syn_pas.c
lu_syntax_EXPORTED_CPPFLAGS = -I$(lu_syntax_DIR)
LIBRARIES += lu_syntax

test_syntax_DIR := $(lu_syntax_DIR)
test_syntax_SRCS = test_syntax.c
test_syntax_LIBS = lu_syntax
EXECUTABLES += test_syntax

define test_syntax_TESTCMD
$(test_syntax_EXEC)
endef
TEST_TARGETS += test_syntax
