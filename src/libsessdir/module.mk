# src/libsessdir/module.mk -- session directory management

lu_sessdir_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_sessdir_SRCS = sessdir.c sessdir_state.c sessdir_layout.c sessdir_watch.c
lu_sessdir_LIBS = lu_core
lu_sessdir_EXPORTED_CPPFLAGS = -I$(lu_sessdir_DIR)
LIBRARIES += lu_sessdir

test_sessdir_DIR := $(lu_sessdir_DIR)
test_sessdir_SRCS = test_sessdir.c
test_sessdir_LIBS = lu_sessdir lu_core
EXECUTABLES += test_sessdir

define test_sessdir_TESTCMD
$(test_sessdir_EXEC)
endef
TEST_TARGETS += test_sessdir
