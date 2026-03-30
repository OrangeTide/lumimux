# src/module.mk -- lumi multi-call binary and project root

lumi_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
lumi_SRCS = multicall.c \
	cmd/attach/attach.c cmd/attach/attach_ui.c cmd/attach/prefix_menu.c \
	cmd/attach/picker.c cmd/attach/apps_menu.c cmd/attach/theme_cfg.c \
	cmd/attach/app_calc.c cmd/attach/app_cal.c cmd/attach/app_emoji.c \
	cmd/attach/app_dict.c cmd/attach/predict.c cmd/attach/color_picker.c \
	cmd/attach/selection.c \
	cmd/attr/attr.c \
	cmd/mserver/mserver.c \
	cmd/new/new.c \
	cmd/list/list.c \
	cmd/version/version.c \
	cmd/kill/kill.c \
	cmd/detach/detach.c \
	cmd/new-window/new_window.c \
	cmd/reload/reload.c \
	cmd/send-input/send_input.c \
	cmd/send-keys/send_keys.c \
	cmd/splash/splash_cmd.c
lumi_LIBS = lu_iox lu_ipc lu_attr lu_sessdir lu_session lu_tio lu_render \
	lu_txl lu_termlib lu_vt lu_utf8 lu_pty lu_keys lu_cfg lu_status \
	lu_tui lu_tui_term lu_wm lu_tile lu_splash lu_core
lumi_LDLIBS = -lutil
lumi_CPPFLAGS = -I$(lumi_DIR)
EXECUTABLES += lumi

LUMI_CMDS = attach attr mserver new list version kill detach new-window \
	reload send-input send-keys splash

.PHONY: symlinks
symlinks: lumi
	@for cmd in $(LUMI_CMDS); do \
		ln -sf lumi $(BINDIR)/lumi-$$cmd; \
	done

SUBDIRS = libcore libutf8 libiox libpty libvt libtio librender libtxl \
	libtermlib libipc libattr libsessdir libsession libkeys libcfg \
	libstatus libsplash libtui libtui_term libwm libtile \
	cmd/attr cmd/mserver cmd/attach cmd/new cmd/list \
	cmd/version cmd/kill cmd/detach cmd/send-keys \
	cmd/send-input cmd/new-window cmd/reload cmd/splash
