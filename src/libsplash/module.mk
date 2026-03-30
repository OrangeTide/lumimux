# src/libsplash/module.mk -- ANSI art splash screen library

lu_splash_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_splash_SRCS = splash.c splash_space.c splash_mountain.c splash_beach.c
lu_splash_LIBS = lu_core lu_vt lu_utf8 lu_tio lu_render
lu_splash_EXPORTED_CPPFLAGS = -I$(lu_splash_DIR)
LIBRARIES += lu_splash

test_splash_DIR := $(lu_splash_DIR)
test_splash_SRCS = test_splash.c
test_splash_LIBS = lu_splash lu_render lu_txl lu_termlib lu_vt lu_utf8 lu_tio lu_core
EXECUTABLES += test_splash

define test_splash_TESTCMD
$(test_splash_EXEC)
endef
TEST_TARGETS += test_splash
