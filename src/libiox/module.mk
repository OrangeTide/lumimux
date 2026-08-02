# src/libiox/module.mk -- libiox, vendored. See VENDORED.md for the version.
#
# Target is lu_iox rather than iox because lumi prefixes its libraries.
# libiox 0.1.0 bundles its own pq.h, so the timer heap no longer comes from
# lu_core; the lu_core dependency is kept for the rest of the tree.

lu_iox_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lu_iox_SRCS = iox_loop.c iox_fd.c iox_signal.c iox_timer.c iox_plat.c
lu_iox_LIBS = lu_core
lu_iox_EXPORTED_CPPFLAGS = -I$(lu_iox_DIR)
LIBRARIES += lu_iox

test_iox_DIR := $(lu_iox_DIR)
test_iox_SRCS = test_iox.c
test_iox_LIBS = lu_iox lu_core
EXECUTABLES += test_iox

define test_iox_TESTCMD
$(test_iox_EXEC)
endef
TEST_TARGETS += test_iox
