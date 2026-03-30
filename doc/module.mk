# doc/module.mk -- documentation targets

doc_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
MANDIR  := $(CURDIR)/_out/man/man1

.PHONY: man
man: $(MANDIR)/lumi.1

$(MANDIR)/lumi.1: $(doc_DIR)index.md | $(MANDIR)/
	pandoc -s -t man $< -o $@

$(MANDIR)/:
	$(MKDIR_P) $@
