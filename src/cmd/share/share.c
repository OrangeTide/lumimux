/* share.c : show and move the keyboard among a session's clients */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "multicall.h"

#include "proxy_broker.h"
#include "sessdir.h"
#include "sessdir_control.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr,
	    "usage: lumi-share [-s name] [-l | -g id | -r id | -k id "
	    "| -a id | -d id | -L | -U | -M mode | -D display]\n");
	fprintf(stderr,
	    "       lumi-share [-s name] -u user | -G group | -B\n");
	fprintf(stderr,
	    "  -l        list the clients attached to the session\n"
	    "  -g id     give the keyboard to this client\n"
	    "  -r id     ask the client holding the keyboard for it\n"
	    "  -k id     ask this client to detach\n"
	    "  -a id     admit this pending (ask) client\n"
	    "  -d id     refuse this pending (ask) client\n"
	    "  -L        close the session to further clients\n"
	    "  -U        open the session to further clients\n"
	    "  -M mode   set share.mode: single-writer or multi-writer\n"
	    "  -D disp   set share.display: independent or shared\n"
	    "  -u user   start the cross-user broker for one named user\n"
	    "  -G group  start the cross-user broker for a group\n"
	    "  -B        stop the cross-user broker\n"
	    "  -s name   session name (default: 0)\n");
}

/* ---- cross-user broker (FUTURE.md 12-b) ---- */

/* Ensure <session>/access exists, seeded with a safe default: the owner
 * keeps write, the new subject gets view, everyone else is denied. Never
 * overwrites an existing file -- once the owner is curating it by hand,
 * this command must not clobber their edits. Returns 0 if the file exists
 * or was created, -1 on error. */
static int
broker_seed_acl(const char *session, const char *subject_line)
{
	char *dir, path[PATH_MAX], content[256];
	struct stat st;
	int fd, n;

	sessdir_session_create(session);
	dir = sessdir_session_path(session);
	if (!dir)
		return -1;
	n = snprintf(path, sizeof(path), "%s/access", dir);
	free(dir);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -1;

	if (stat(path, &st) == 0)
		return 0;		/* already there: leave it alone */

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return (errno == EEXIST) ? 0 : -1;
	n = snprintf(content, sizeof(content), "owner write\n%s\n* deny\n",
	    subject_line);
	if (n < 0 || (size_t)n >= sizeof(content) ||
	    write(fd, content, (size_t)n) != n) {
		close(fd);
		unlink(path);
		return -1;
	}
	close(fd);
	chmod(path, 0600);
	return 0;
}

/* The broker's own pid, if its pidfile names a still-live process, else
 * -1. A pidfile left behind by a broker that crashed or was killed rather
 * than shut down cleanly names a dead pid and is treated as "not running"
 * rather than an error. */
static int
broker_running_pid(const char *session)
{
	char path[PATH_MAX];
	FILE *f;
	long pid = 0;

	if (proxy_broker_pidfile_path(session, path, sizeof(path)) < 0)
		return -1;
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%ld", &pid) != 1)
		pid = 0;
	fclose(f);
	if (pid <= 0 || kill((pid_t)pid, 0) < 0)
		return -1;
	return (int)pid;
}

/* Start the broker as a background daemon: fork, detach from the
 * controlling terminal, and run lumi-proxy's -L listener in-process (this
 * is the same lumi binary, so no exec is needed). Returns once the child
 * has either published its pidfile or a few seconds have passed without
 * one, so the caller's exit status reflects whether it actually started. */
static int
broker_start(const char *session, const char *user, const char *group)
{
	pid_t pid;
	int waited;

	if (broker_running_pid(session) > 0) {
		fprintf(stderr, "lumi-share: broker already running for "
		    "session '%s'\n", session);
		return 1;
	}

	if (group) {
		char line[128];

		snprintf(line, sizeof(line), "group:%s\tview", group);
		if (broker_seed_acl(session, line) < 0) {
			fprintf(stderr,
			    "lumi-share: could not seed the acl\n");
			return 1;
		}
	} else if (user) {
		char line[128];

		snprintf(line, sizeof(line), "user:%s\tview", user);
		if (broker_seed_acl(session, line) < 0) {
			fprintf(stderr,
			    "lumi-share: could not seed the acl\n");
			return 1;
		}
	}

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "lumi-share: fork failed: %s\n",
		    strerror(errno));
		return 1;
	}
	if (pid == 0) {
		int nfd;

		setsid();
		nfd = open("/dev/null", O_RDWR);
		if (nfd >= 0) {
			dup2(nfd, STDIN_FILENO);
			dup2(nfd, STDOUT_FILENO);
			dup2(nfd, STDERR_FILENO);
			if (nfd > STDERR_FILENO)
				close(nfd);
		}
		if (group) {
			char *bargv[] = { "lumi-proxy", "-s", (char *)session,
			    "-L", "-g", (char *)group, NULL };

			optind = 1;
			_exit(cmd_proxy_main(6, bargv));
		} else {
			char *bargv[] = { "lumi-proxy", "-s", (char *)session,
			    "-L", NULL };

			optind = 1;
			_exit(cmd_proxy_main(4, bargv));
		}
	}

	/* the child is its own session leader now; nothing left for this
	 * process to do but confirm it actually came up before reporting
	 * success. */
	for (waited = 0; waited < 2000 && broker_running_pid(session) <= 0;
	    waited += 20)
		usleep(20000);
	if (broker_running_pid(session) <= 0) {
		fprintf(stderr, "lumi-share: broker did not start\n");
		return 1;
	}
	printf("broker listening for session '%s' (%s)\n", session,
	    group ? "group-gated" : "world-reachable, peer-credential gated");
	return 0;
}

static int
broker_stop_cmd(const char *session)
{
	int pid = broker_running_pid(session);

	if (pid <= 0) {
		fprintf(stderr, "lumi-share: no broker running for session "
		    "'%s'\n", session);
		return 1;
	}
	kill((pid_t)pid, SIGTERM);
	printf("stopping broker for session '%s'\n", session);
	return 0;
}

/* how long a client has been attached, in a form worth reading */
static void
fmt_age(long since, char *buf, size_t bufsz)
{
	long secs = (long)time(NULL) - since;

	if (since <= 0 || secs < 0) {
		snprintf(buf, bufsz, "-");
		return;
	}
	if (secs < 60)
		snprintf(buf, bufsz, "%lds", secs);
	else if (secs < 3600)
		snprintf(buf, bufsz, "%ldm", secs / 60);
	else
		snprintf(buf, bufsz, "%ldh%ldm", secs / 3600,
		    (secs % 3600) / 60);
}

static int
do_list(const char *session)
{
	struct sessdir_client list[SESSDIR_CLIENT_MAX];
	char holder[64];
	int n, i;

	sessdir_cleanup_stale(session);

	n = sessdir_client_list(session, list, SESSDIR_CLIENT_MAX);
	if (n < 0) {
		fprintf(stderr, "lumi-share: session '%s' not found\n",
		    session);
		return 1;
	}
	if (n == 0) {
		printf("no clients attached to session '%s'\n", session);
		return 0;
	}

	if (sessdir_token_holder(session, holder, sizeof(holder), NULL) == 0)
		printf("keyboard: %s\n", holder[0] ? holder : "(unnamed)");
	else
		printf("keyboard: nobody\n");
	if (sessdir_share_locked(session))
		printf("locked: no further clients may attach\n");

	printf("%-10s %-24s %-12s %-6s %-8s %-8s %-9s %-8s %s\n", "ID", "NAME",
	    "WHO", "ROLE", "MODE", "COUPLING", "SIZE", "PENDING", "ATTACHED");
	for (i = 0; i < n; i++) {
		char size[16], age[16];

		if (list[i].cols > 0 && list[i].rows > 0)
			snprintf(size, sizeof(size), "%dx%d", list[i].cols,
			    list[i].rows);
		else
			snprintf(size, sizeof(size), "-");
		fmt_age(list[i].since, age, sizeof(age));
		printf("%-10lu %-24s %-12s %-6s %-8s %-8s %-9s %-8s %s\n",
		    list[i].client_id, list[i].name,
		    list[i].who[0] ? list[i].who : "-", list[i].role,
		    list[i].mode, list[i].coupling[0] ? list[i].coupling : "-",
		    size, list[i].pending ? "yes" : "-", age);
	}
	return 0;
}

/* Look up an attached client.
 *
 * Refuses an id that is not attached, since a message addressed to a
 * client that does not exist would just sit there unread. Also hands back
 * the client's name, so a request carries who is asking rather than
 * arriving anonymously on someone else's screen.
 */
static int
client_lookup(const char *session, unsigned long id, char *name, size_t namesz)
{
	struct sessdir_client list[SESSDIR_CLIENT_MAX];
	int n, i;

	if (name && namesz > 0)
		name[0] = '\0';
	n = sessdir_client_list(session, list, SESSDIR_CLIENT_MAX);
	for (i = 0; i < n; i++) {
		if (list[i].client_id != id)
			continue;
		if (name && namesz > 0)
			snprintf(name, namesz, "%s", list[i].name);
		return 1;
	}
	return 0;
}

int
cmd_share_main(int argc, char **argv)
{
	const char *session = "0";
	int verb = SESSDIR_CTL_NONE;
	int list = 0, lock = -1, broker_off = 0;
	const char *broker_user = NULL, *broker_group = NULL;
	const char *share_mode = NULL;
	const char *share_display = NULL;
	unsigned long target = 0;
	int opt;

	while ((opt = getopt(argc, argv, "s:lg:r:k:a:d:LUM:D:u:G:B")) != -1) {
		switch (opt) {
		case 's':
			session = optarg;
			break;
		case 'l':
			list = 1;
			break;
		case 'g':
			verb = SESSDIR_CTL_GRANT;
			target = strtoul(optarg, NULL, 10);
			break;
		case 'r':
			verb = SESSDIR_CTL_REQUEST;
			target = strtoul(optarg, NULL, 10);
			break;
		case 'k':
			verb = SESSDIR_CTL_KICK;
			target = strtoul(optarg, NULL, 10);
			break;
		case 'a':
			verb = SESSDIR_CTL_APPROVE;
			target = strtoul(optarg, NULL, 10);
			break;
		case 'd':
			verb = SESSDIR_CTL_REJECT;
			target = strtoul(optarg, NULL, 10);
			break;
		case 'L':
			lock = 1;
			break;
		case 'U':
			lock = 0;
			break;
		case 'M':
			share_mode = optarg;
			break;
		case 'D':
			share_display = optarg;
			break;
		case 'u':
			broker_user = optarg;
			break;
		case 'G':
			broker_group = optarg;
			break;
		case 'B':
			broker_off = 1;
			break;
		default:
			usage();
			return 1;
		}
	}

	if (broker_user && broker_group) {
		fprintf(stderr, "lumi-share: -u and -G are exclusive\n");
		return 1;
	}
	if ((broker_user || broker_group) && broker_off) {
		fprintf(stderr, "lumi-share: -u/-G and -B are exclusive\n");
		return 1;
	}
	if (broker_off)
		return broker_stop_cmd(session);
	if (broker_user || broker_group)
		return broker_start(session, broker_user, broker_group);

	if (!list && verb == SESSDIR_CTL_NONE && lock < 0 && !share_mode &&
	    !share_display)
		list = 1;			/* the useful default */

	if (lock >= 0) {
		if (sessdir_share_set_locked(session, lock) < 0) {
			fprintf(stderr, "lumi-share: session '%s' not found\n",
			    session);
			return 1;
		}
		printf("session '%s' is now %s\n", session,
		    lock ? "closed to further clients"
		    : "open to further clients");
	}

	if (share_mode) {
		int mode;

		if (strcmp(share_mode, "single-writer") == 0)
			mode = SESSDIR_SHARE_SINGLE_WRITER;
		else if (strcmp(share_mode, "multi-writer") == 0)
			mode = SESSDIR_SHARE_MULTI_WRITER;
		else {
			fprintf(stderr, "lumi-share: -M takes single-writer "
			    "or multi-writer, not '%s'\n", share_mode);
			return 1;
		}
		if (sessdir_share_mode_set(session, mode) < 0) {
			fprintf(stderr, "lumi-share: could not set share "
			    "mode for session '%s'\n", session);
			return 1;
		}
		printf("session '%s' is now %s\n", session, share_mode);
	}

	if (share_display) {
		int display;

		if (strcmp(share_display, "independent") == 0)
			display = SESSDIR_SHARE_DISPLAY_INDEPENDENT;
		else if (strcmp(share_display, "shared") == 0)
			display = SESSDIR_SHARE_DISPLAY_SHARED;
		else {
			fprintf(stderr, "lumi-share: -D takes independent "
			    "or shared, not '%s'\n", share_display);
			return 1;
		}
		if (sessdir_share_display_set(session, display) < 0) {
			fprintf(stderr, "lumi-share: could not set share "
			    "display for session '%s'\n", session);
			return 1;
		}
		printf("session '%s' display is now %s\n", session,
		    share_display);
	}

	if (verb != SESSDIR_CTL_NONE) {
		char who[64];

		if (target == 0) {
			fprintf(stderr, "lumi-share: not a client id\n");
			return 1;
		}
		if (!client_lookup(session, target, who, sizeof(who))) {
			fprintf(stderr, "lumi-share: no client %lu attached "
			    "to session '%s'\n", target, session);
			return 1;
		}
		/* a request is posted on behalf of the target: the client
		 * holding the keyboard is the one that answers it */
		if (sessdir_ctl_post(session, verb, target,
		    verb == SESSDIR_CTL_REQUEST ? target : 0, who) < 0) {
			fprintf(stderr, "lumi-share: could not post to "
			    "session '%s'\n", session);
			return 1;
		}
	}

	if (list)
		return do_list(session);
	return 0;
}
