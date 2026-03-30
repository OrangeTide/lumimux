# src/libcore/module.mk -- common utilities (allocation, logging, paths)

lu_core_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_core_SRCS = xmalloc.c str.c log.c path.c daemonize.c dbg.c
lu_core_EXPORTED_CPPFLAGS = -I$(lu_core_DIR)
LIBRARIES += lu_core
