/* send_input.c : send arbitrary input to a pane */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "multicall.h"

#include "ipc.h"
#include "ipc_msg.h"
#include "sessdir.h"
#include "sessdir_state.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char *progname = "lumi-send-input";

/* Bound the handshake so one unresponsive window cannot hang the tool. */
#define RECV_TIMEOUT_SEC 5

#define MAX_WINDOWS 64

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-s session] [-w pid | -i index | -o] [--] [bytes...]\n"
	    "\n"
	    "Inject raw bytes into a session window, as if typed. With no byte\n"
	    "arguments, the input is read from standard input. Arguments are\n"
	    "joined with single spaces and sent verbatim; no newline is added.\n"
	    "\n"
	    "  -s session  target session (default $LUMI_SESSION or 0)\n"
	    "  -w pid      target the window with this server pid\n"
	    "  -i index    target the window with this number (as in the tab bar)\n"
	    "  -o          target a window other than the focused one\n",
	    progname);
}

/* Resolve which server pid to send to. target > 0 is explicit; 0 is the
 * focused window; -1 is a non-focused ("other") window, preferring
 * $LUMI_SEND_TARGET when it names a live one. Returns a pid, or 0 for none. */
static pid_t
resolve_target(const char *session, pid_t target, pid_t focus)
{
	pid_t pids[MAX_WINDOWS];
	const char *envt;
	int n, i;
	pid_t want;

	if (target > 0)
		return target;
	if (target == 0)
		return focus;			/* focused window */

	/* target < 0: any window that is not the focused one */
	n = sessdir_list_servers(session, pids, MAX_WINDOWS);
	if (n < 0)
		n = 0;

	envt = getenv("LUMI_SEND_TARGET");
	if (envt && *envt) {
		want = (pid_t)strtol(envt, NULL, 10);
		for (i = 0; i < n; i++)
			if (pids[i] == want)
				return want;
	}
	for (i = 0; i < n; i++)
		if (pids[i] != focus)
			return pids[i];
	return 0;
}

/* Resolve a window number (as shown in the tab bar, 0-based) to its server
 * pid. The stable numbering lives in the session state's slot map, where the
 * slot index is the window number and an empty slot holds 0. Returns the pid,
 * or 0 when the number is out of range or names an empty slot. */
pid_t
lu_window_pid(const char *session, int index)
{
	struct sessdir_state *st;
	pid_t nums[MAX_WINDOWS];
	pid_t pid = 0;
	int n;

	if (!session || index < 0)
		return 0;

	sessdir_cleanup_stale(session);
	st = sessdir_state_open(session);
	if (!st)
		return 0;
	n = sessdir_state_nums(st, nums, MAX_WINDOWS);
	if (index < n && nums[index] != 0)
		pid = nums[index];
	sessdir_state_close(st);
	return pid;
}

/* Send the payload as one bracketed input run, chunked to the wire limit. The
 * mserver buffers the run and flushes it to the PTY as a single write, so the
 * injection cannot interleave with another writer or be split mid-sequence. */
static int
send_run(int fd, const char *data, size_t len)
{
	size_t off = 0;

	if (ipc_msg_send_empty(fd, IPC_MSG_INPUT_BEGIN) < 0)
		return -1;
	while (off < len) {
		size_t chunk = len - off;

		if (chunk > IPC_MAX_PAYLOAD)
			chunk = IPC_MAX_PAYLOAD;
		if (ipc_msg_send(fd, IPC_MSG_INPUT, data + off,
		    (uint32_t)chunk) < 0)
			return -1;
		off += chunk;
	}
	if (ipc_msg_send_empty(fd, IPC_MSG_INPUT_END) < 0)
		return -1;
	return 0;
}

enum lu_send_result
lu_send_input(const char *session, pid_t target, const char *data, size_t len)
{
	struct sessdir_state *st;
	pid_t focus, pid;
	char *dir, path[PATH_MAX];
	uint8_t role;
	int fd;
	struct timeval tv = { RECV_TIMEOUT_SEC, 0 };

	if (!session || len == 0)
		return LU_SEND_OK;		/* nothing to do */

	sessdir_cleanup_stale(session);
	st = sessdir_state_open(session);
	if (!st)
		return LU_SEND_NO_SESSION;
	focus = sessdir_state_focus(st);
	sessdir_state_close(st);

	pid = resolve_target(session, target, focus);
	if (pid == 0)
		return LU_SEND_NO_TARGET;

	dir = sessdir_server_path(session, pid);
	if (!dir)
		return LU_SEND_ERROR;
	if (snprintf(path, sizeof(path), "%s/socket", dir)
	    >= (int)sizeof(path)) {
		free(dir);
		return LU_SEND_ERROR;
	}
	free(dir);

	fd = ipc_connect(path);
	if (fd < 0)
		return LU_SEND_ERROR;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (ipc_client_attach(fd, IPC_ATTACH_F_SIZE_OBSERVE, "send-input",
	    &role) < 0) {
		ipc_close(fd);
		return LU_SEND_ERROR;
	}
	if (role != IPC_ROLE_WRITE) {
		ipc_close(fd);
		return LU_SEND_READONLY;
	}
	if (send_run(fd, data, len) < 0) {
		ipc_close(fd);
		return LU_SEND_ERROR;
	}

	/* Deliver our input, then EOF, in that order: shutdown() flushes the
	 * queued INPUT messages before the FIN, so the server reads the whole
	 * run before it sees us leave. Keep the read side open and drain the
	 * server's screen replay -- if our read end were already closed, the
	 * server's write to us would fail and it would drop the client before
	 * flushing the run to the PTY. */
	shutdown(fd, SHUT_WR);
	{
		char rbuf[4096];
		uint32_t rtype, rlen;

		while (ipc_msg_recv(fd, &rtype, rbuf, sizeof(rbuf), &rlen) == 0)
			;
	}
	ipc_close(fd);
	return LU_SEND_OK;
}

/* Read all of standard input into a freshly allocated buffer. Returns the
 * buffer (caller frees) with *out_len set, or NULL on allocation failure. */
static char *
read_stdin(size_t *out_len)
{
	char *buf = NULL;
	size_t len = 0, cap = 0;

	for (;;) {
		size_t want;
		ssize_t n;

		if (len == cap) {
			size_t ncap = cap ? cap * 2 : 4096;
			char *nb = realloc(buf, ncap);

			if (!nb) {
				free(buf);
				return NULL;
			}
			buf = nb;
			cap = ncap;
		}
		want = cap - len;
		n = read(STDIN_FILENO, buf + len, want);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return NULL;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	*out_len = len;
	return buf ? buf : calloc(1, 1);
}

/* Join the argument vector into one buffer with single-space separators. */
static char *
join_args(int argc, char **argv, size_t *out_len)
{
	size_t total = 0, off = 0;
	char *buf;
	int i;

	for (i = 0; i < argc; i++)
		total += strlen(argv[i]);
	total += (size_t)(argc - 1);		/* separators */

	buf = malloc(total + 1);
	if (!buf)
		return NULL;
	for (i = 0; i < argc; i++) {
		size_t l = strlen(argv[i]);

		if (i > 0)
			buf[off++] = ' ';
		memcpy(buf + off, argv[i], l);
		off += l;
	}
	buf[off] = '\0';
	*out_len = off;
	return buf;
}

int
cmd_send_input_main(int argc, char **argv)
{
	const char *session = NULL;
	char *data;
	size_t len = 0;
	pid_t target = 0;			/* focused window by default */
	int opt, rc = 0, index = -1;

	if (argv[0])
		progname = argv[0];

	while ((opt = getopt(argc, argv, "s:w:i:o")) != -1) {
		switch (opt) {
		case 's':
			session = optarg;
			break;
		case 'w':
			target = (pid_t)strtol(optarg, NULL, 10);
			if (target <= 0) {
				fprintf(stderr, "%s: bad window pid '%s'\n",
				    progname, optarg);
				return 1;
			}
			break;
		case 'i': {
			char *end;

			index = (int)strtol(optarg, &end, 10);
			if (*end != '\0' || index < 0) {
				fprintf(stderr, "%s: bad window index '%s'\n",
				    progname, optarg);
				return 1;
			}
			break;
		}
		case 'o':
			target = -1;		/* a non-focused window */
			break;
		default:
			usage();
			return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (!session) {
		session = getenv("LUMI_SESSION");
		if (!session)
			session = "0";
	}

	if (index >= 0) {			/* -i resolves to a server pid */
		target = lu_window_pid(session, index);
		if (target == 0) {
			fprintf(stderr, "%s: no window numbered %d in session "
			    "'%s'\n", progname, index, session);
			return 1;
		}
	}

	if (argc > 0)
		data = join_args(argc, argv, &len);
	else
		data = read_stdin(&len);
	if (!data) {
		fprintf(stderr, "%s: out of memory\n", progname);
		return 1;
	}
	if (len == 0) {
		free(data);
		return 0;			/* nothing to send */
	}

	switch (lu_send_input(session, target, data, len)) {
	case LU_SEND_OK:
		break;
	case LU_SEND_NO_SESSION:
		fprintf(stderr, "%s: session '%s' not found\n", progname,
		    session);
		rc = 1;
		break;
	case LU_SEND_NO_TARGET:
		fprintf(stderr, "%s: no target window\n", progname);
		rc = 1;
		break;
	case LU_SEND_READONLY:
		fprintf(stderr, "%s: window is read-only; another client holds "
		    "the keyboard\n", progname);
		rc = 1;
		break;
	case LU_SEND_ERROR:
		fprintf(stderr, "%s: sending input failed\n", progname);
		rc = 1;
		break;
	}

	free(data);
	return rc;
}
