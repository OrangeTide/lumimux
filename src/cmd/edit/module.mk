# src/cmd/edit/module.mk -- test suite for the lumi edit editor
#
# edit.c and vi.c are compiled into the lumi multi-call binary (via lumi_SRCS
# in src/module.mk), not into a library. test_edit.c includes both directly to
# reach their internals, so it links the same libraries and include path the
# editor uses inside lumi. lu_send_input is stubbed in the test, since it lives
# in the send-input command source rather than a library.

test_edit_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
test_edit_SRCS = test_edit.c
test_edit_CPPFLAGS = -I$(lumi_DIR)
test_edit_LIBS = $(lumi_LIBS)
test_edit_LDLIBS = $(lumi_LDLIBS)
EXECUTABLES += test_edit

define test_edit_TESTCMD
$(test_edit_EXEC)
endef
TEST_TARGETS += test_edit
