# src/cmd/attach/module.mk -- connect to server, relay terminal I/O

test_input_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
test_input_SRCS = test_input.c
test_input_LIBS = lu_keys lu_termlib lu_cfg lu_core
EXECUTABLES += test_input

define test_input_TESTCMD
$(test_input_EXEC)
endef
TEST_TARGETS += test_input

lu_predict_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_predict_SRCS = predict.c
lu_predict_LIBS = lu_vt lu_utf8 lu_core
lu_predict_EXPORTED_CPPFLAGS = -I$(lu_predict_DIR)
LIBRARIES += lu_predict

test_predict_DIR := $(lu_predict_DIR)
test_predict_SRCS = test_predict.c
test_predict_LIBS = lu_predict lu_vt lu_utf8 lu_core
EXECUTABLES += test_predict

define test_predict_TESTCMD
$(test_predict_EXEC)
endef
TEST_TARGETS += test_predict
