# src/module.mk -- lumi multi-call binary and project root

lumi_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lumi_SRCS = multicall.c \
	cmd/attach/attach.c cmd/attach/attach_ui.c cmd/attach/prefix_menu.c \
	cmd/attach/picker.c cmd/attach/apps_menu.c cmd/attach/theme_cfg.c \
	cmd/attach/app_calc.c cmd/attach/app_cal.c cmd/attach/app_emoji.c \
	cmd/attach/app_dict.c cmd/attach/color_picker.c \
	cmd/attach/session_picker.c cmd/attach/selection.c \
	cmd/attach/share_menu.c cmd/attach/gpm_mouse.c \
	cmd/attr/attr.c \
	cmd/new/new.c \
	cmd/list/list.c \
	cmd/version/version.c \
	cmd/kill/kill.c \
	cmd/detach/detach.c \
	cmd/new-window/new_window.c \
	cmd/net-keygen/net_keygen.c \
	cmd/net-passwd/net_passwd.c \
	cmd/reload/reload.c \
	cmd/send-input/send_input.c \
	cmd/send-keys/send_keys.c \
	cmd/share/share.c \
	cmd/splash/splash_cmd.c
lumi_LIBS = lu_mserver lu_iox lu_ipc lu_net lu_attr lu_sessdir lu_session \
	lu_tio lu_render lu_txl lu_termlib lu_vt lu_utf8 lu_pty lu_keys lu_cfg \
	lu_taskbar lu_tui lu_tui_term lu_wm lu_tile lu_splash lu_predict \
	lu_netproxy lu_proxy lu_core
lumi_LDLIBS = $(if $(findstring darwin,$(TARGET_TRIPLET)),,-lutil)
lumi_CPPFLAGS = -I$(lumi_DIR)

# Optional GPM support: mouse input on the raw Linux text console.
# libgpm is auto-detected with a small compile-and-link probe, the way an
# autoconf script would. Override with GPM=1 (force on) or GPM=0 (force off).
#
# A literal '#' in the probe would start a comment on Make 3.81, which treats
# '#' as a comment even inside $(shell ...) and swallows the rest of the line.
# Emit it through $(gpm_hash) so no '#' appears on the $(shell) line.
gpm_hash := \#
ifeq ($(origin GPM),undefined)
GPM := $(shell printf '$(gpm_hash)include <gpm.h>\nint main(void){return Gpm_Close();}\n' | $(CC) -x c - -lgpm -o /dev/null >/dev/null 2>&1 && echo 1)
endif
ifeq ($(GPM),1)
lumi_CPPFLAGS += -DHAVE_GPM
lumi_LDLIBS += -lgpm
endif

EXECUTABLES += lumi

LUMI_CMDS = attach attr mserver new list version kill detach new-window \
	proxy net-proxy net-keygen net-passwd reload send-input send-keys share \
	splash

.PHONY: symlinks clean-symlinks
symlinks: lumi
	@for cmd in $(LUMI_CMDS); do \
		ln -sf lumi $(BINDIR)/lumi-$$cmd; \
	done
clean-symlinks:
	for cmd in $(LUMI_CMDS); do $(RM) $(BINDIR)/lumi-$$cmd; done
clean: clean-symlinks

SUBDIRS = libcore libutf8 libiox libpty libvt libtio librender libtxl \
	libtermlib libipc libnet libattr libsessdir libsession libkeys libcfg \
	libtaskbar libsplash libtui libtui_term libwm libtile \
	cmd/attr cmd/mserver cmd/attach cmd/new cmd/list \
	cmd/proxy cmd/net-proxy cmd/version cmd/kill cmd/detach cmd/send-keys \
	cmd/share \
	cmd/send-input cmd/new-window cmd/reload cmd/splash
