# src/libtui_term/module.mk -- terminal backend for libtui

lu_tui_term_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_tui_term_SRCS = tui_term.c
lu_tui_term_LIBS = lu_tui lu_txl lu_tio lu_termlib lu_vt lu_utf8 lu_core
lu_tui_term_EXPORTED_CPPFLAGS = -I$(lu_tui_term_DIR)
LIBRARIES += lu_tui_term
