# src/libtio/module.mk -- terminal I/O (raw mode, buffered output)

lu_tio_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_tio_SRCS = tio.c tio_write.c
lu_tio_LIBS = lu_core
lu_tio_EXPORTED_CPPFLAGS = -I$(lu_tio_DIR)
LIBRARIES += lu_tio
