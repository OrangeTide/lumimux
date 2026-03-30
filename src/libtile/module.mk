# src/libtile/module.mk -- tiled split-pane compositor

lu_tile_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_tile_SRCS = tile.c tile_composite.c
lu_tile_LIBS = lu_vt lu_utf8 lu_core
lu_tile_EXPORTED_CPPFLAGS = -I$(lu_tile_DIR)
LIBRARIES += lu_tile

test_tile_DIR := $(lu_tile_DIR)
test_tile_SRCS = test_tile.c
test_tile_LIBS = lu_tile lu_vt lu_utf8 lu_core
EXECUTABLES += test_tile

define test_tile_TESTCMD
$(test_tile_EXEC)
endef
TEST_TARGETS += test_tile
