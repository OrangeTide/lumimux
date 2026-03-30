# src/libutf8/module.mk -- UTF-8 encoding/decoding and character width

lu_utf8_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_utf8_SRCS = utf8.c rune_width.c width_tables.c
lu_utf8_EXPORTED_CPPFLAGS = -I$(lu_utf8_DIR)
LIBRARIES += lu_utf8

test_utf8_DIR := $(lu_utf8_DIR)
test_utf8_SRCS = test_utf8.c
test_utf8_LIBS = lu_utf8
EXECUTABLES += test_utf8

define test_utf8_TESTCMD
$(test_utf8_EXEC)
endef
TEST_TARGETS += test_utf8

.PHONY: gen-width-tables
gen-width-tables:
	$(lu_utf8_DIR)gen-width-tables.sh
