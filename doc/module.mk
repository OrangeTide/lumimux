# doc/module.mk -- documentation targets

doc_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
MANDIR  := $(CURDIR)/_out/man/man1

# Version string for @VERSION@ substitution, read from the C header so the
# manual page and the binary never disagree.
LUMI_VERSION := $(shell sed -n 's/.*LUMI_VERSION "\([^"]*\)".*/\1/p' src/version.h)

# Render the manual page from its template, substituting the version and an
# install prefix.  Usage: $(call gen_man,<prefix>,<outfile>)
gen_man = sed -e 's|@VERSION@|$(LUMI_VERSION)|g' -e 's|@PREFIX@|$1|g' \
	doc/lumi.1.in > $2

.PHONY: man
man: $(MANDIR)/lumi.1

# Preview build: uses the default install prefix.  The install target
# regenerates with its own PREFIX (see src/module.mk), so what ships always
# names the directory it was installed into.
$(MANDIR)/lumi.1: doc/lumi.1.in src/version.h | $(MANDIR)/
	$(call gen_man,$(PREFIX),$@)

$(MANDIR)/:
	$(MKDIR_P) $@
