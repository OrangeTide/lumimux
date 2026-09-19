# src/libtext/module.mk -- editable text buffer with a line index

lu_text_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_text_SRCS = text.c
lu_text_EXPORTED_CPPFLAGS = -I$(lu_text_DIR)
LIBRARIES += lu_text

test_text_DIR := $(lu_text_DIR)
test_text_SRCS = test_text.c
test_text_LIBS = lu_text
EXECUTABLES += test_text

define test_text_TESTCMD
$(test_text_EXEC)
endef
TEST_TARGETS += test_text
