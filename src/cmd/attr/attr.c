/* attr.c : get, set, delete, and list mserver attributes */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"

#include "ipc.h"
#include "ipc_attr.h"
#include "ipc_msg.h"
#include "sessdir.h"
#include "sessdir_state.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr,
	    "usage: lumi-attr [-s name] get <key>\n"
	    "       lumi-attr [-s name] set <key> <value>\n"
	    "       lumi-attr [-s name] delete <key>\n"
	    "       lumi-attr [-s name] list\n");
}

/* connect to the focused mserver's socket in the given session.
 * returns fd on success, -1 on error. */
static int
connect_focused(const char *session)
{
	struct sessdir_state *st;
	pid_t focus;
	char *dir, path[PATH_MAX];
	int fd;

	sessdir_cleanup_stale(session);

	st = sessdir_state_open(session);
	if (!st) {
		fprintf(stderr, "lumi-attr: session '%s' not found\n",
		    session);
		return -1;
	}

	focus = sessdir_state_focus(st);
	sessdir_state_close(st);

	if (focus == 0) {
		fprintf(stderr, "lumi-attr: no focused window\n");
		return -1;
	}

	dir = sessdir_server_path(session, focus);
	if (!dir)
		return -1;
	if (snprintf(path, sizeof(path), "%s/socket", dir)
	    >= (int)sizeof(path)) {
		free(dir);
		return -1;
	}
	free(dir);

	fd = ipc_connect(path);
	if (fd < 0) {
		fprintf(stderr, "lumi-attr: cannot connect to server\n");
		return -1;
	}
	return fd;
}

static int
do_get(int fd, const char *key)
{
	uint32_t txn_id;
	char value[4096];
	int rc = 1;

	if (ipc_attr_txn_begin(fd, &txn_id) < 0) {
		fprintf(stderr, "lumi-attr: txn_begin failed\n");
		return 1;
	}
	if (ipc_attr_get(fd, txn_id, key, value, (int)sizeof(value)) < 0) {
		fprintf(stderr, "lumi-attr: get failed\n");
		ipc_attr_txn_rollback(fd, txn_id);
		return 1;
	}
	printf("%s\n", value);
	rc = 0;

	ipc_attr_txn_rollback(fd, txn_id);
	return rc;
}

static int
do_set(int fd, const char *key, const char *value)
{
	uint32_t txn_id;

	if (ipc_attr_txn_begin(fd, &txn_id) < 0) {
		fprintf(stderr, "lumi-attr: txn_begin failed\n");
		return 1;
	}
	if (ipc_attr_set(fd, txn_id, key, value) < 0) {
		fprintf(stderr, "lumi-attr: set failed\n");
		ipc_attr_txn_rollback(fd, txn_id);
		return 1;
	}
	if (ipc_attr_txn_commit(fd, txn_id) < 0) {
		fprintf(stderr, "lumi-attr: commit failed\n");
		return 1;
	}
	return 0;
}

static int
do_delete(int fd, const char *key)
{
	uint32_t txn_id;

	if (ipc_attr_txn_begin(fd, &txn_id) < 0) {
		fprintf(stderr, "lumi-attr: txn_begin failed\n");
		return 1;
	}
	if (ipc_attr_delete(fd, txn_id, key) < 0) {
		fprintf(stderr, "lumi-attr: delete failed\n");
		ipc_attr_txn_rollback(fd, txn_id);
		return 1;
	}
	if (ipc_attr_txn_commit(fd, txn_id) < 0) {
		fprintf(stderr, "lumi-attr: commit failed\n");
		return 1;
	}
	return 0;
}

static int
do_list(int fd)
{
	uint32_t txn_id;
	char buf[IPC_MAX_PAYLOAD];

	if (ipc_attr_txn_begin(fd, &txn_id) < 0) {
		fprintf(stderr, "lumi-attr: txn_begin failed\n");
		return 1;
	}
	if (ipc_attr_list(fd, txn_id, buf, (int)sizeof(buf)) < 0) {
		fprintf(stderr, "lumi-attr: list failed\n");
		ipc_attr_txn_rollback(fd, txn_id);
		return 1;
	}
	ipc_attr_txn_rollback(fd, txn_id);

	if (buf[0] != '\0')
		printf("%s", buf);
	return 0;
}

int
cmd_attr_main(int argc, char **argv)
{
	const char *session = NULL;
	const char *subcmd;
	int opt, fd, rc;

	while ((opt = getopt(argc, argv, "s:")) != -1) {
		switch (opt) {
		case 's':
			session = optarg;
			break;
		default:
			usage();
			return 1;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage();
		return 1;
	}

	if (!session) {
		session = getenv("LUMI_SESSION");
		if (!session)
			session = "0";
	}

	subcmd = argv[0];

	fd = connect_focused(session);
	if (fd < 0)
		return 1;

	if (strcmp(subcmd, "get") == 0) {
		if (argc != 2) {
			fprintf(stderr, "usage: lumi-attr get <key>\n");
			rc = 1;
		} else {
			rc = do_get(fd, argv[1]);
		}
	} else if (strcmp(subcmd, "set") == 0) {
		if (argc != 3) {
			fprintf(stderr, "usage: lumi-attr set <key> <value>\n");
			rc = 1;
		} else {
			rc = do_set(fd, argv[1], argv[2]);
		}
	} else if (strcmp(subcmd, "delete") == 0) {
		if (argc != 2) {
			fprintf(stderr, "usage: lumi-attr delete <key>\n");
			rc = 1;
		} else {
			rc = do_delete(fd, argv[1]);
		}
	} else if (strcmp(subcmd, "list") == 0) {
		if (argc != 1) {
			fprintf(stderr, "usage: lumi-attr list\n");
			rc = 1;
		} else {
			rc = do_list(fd);
		}
	} else {
		fprintf(stderr, "lumi-attr: unknown command '%s'\n", subcmd);
		usage();
		rc = 1;
	}

	ipc_close(fd);
	return rc;
}
