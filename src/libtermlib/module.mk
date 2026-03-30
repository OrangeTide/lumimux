# src/libtermlib/module.mk -- vendored terminfo + keyboard input (aux01/termlib)
# MIT license, see COPYING in this directory.

lu_termlib_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_termlib_SRCS = ti.c tkbd.c
lu_termlib_CPPFLAGS = -D_XOPEN_SOURCE=700
lu_termlib_EXPORTED_CPPFLAGS = -I$(lu_termlib_DIR)
LIBRARIES += lu_termlib
