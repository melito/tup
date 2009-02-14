#include "server.h"
#include "file.h"
#include "debug.h"
#include "getexecwd.h"
#include "fileio.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/socket.h>

static void *message_thread(void *arg);

static char ldpreload_path[PATH_MAX];

int server_init(void)
{
	if(snprintf(ldpreload_path, sizeof(ldpreload_path), "%s/ldpreload.so",
		    getexecwd()) >= (signed)sizeof(ldpreload_path)) {
		fprintf(stderr, "Error: path for ldpreload.so library is too "
			"long.\n");
		return -1;
	}
	return 0;
}

void server_setenv(struct server *s)
{
	char fd_name[32];
	snprintf(fd_name, sizeof(fd_name), "%i", s->sd[1]);
	fd_name[31] = 0;
	setenv(SERVER_NAME, fd_name, 1);
	setenv("LD_PRELOAD", ldpreload_path, 1);
}

int start_server(struct server *s)
{
	if(socketpair(AF_UNIX, SOCK_DGRAM, 0, s->sd) < 0) {
		perror("pipe");
		return -1;
	}

	init_file_info(&s->finfo);

	if(pthread_create(&s->tid, NULL, message_thread, s) < 0) {
		perror("pthread_create");
		close(s->sd[0]);
		close(s->sd[1]);
		return -1;
	}

	return 0;
}

int stop_server(struct server *s)
{
	void *retval = NULL;
	enum access_type at = ACCESS_STOP_SERVER;
	int rc;

	rc = send(s->sd[1], &at, sizeof(at), 0);
	if(rc != sizeof(at)) {
		perror("write");
		return -1;
	}
	pthread_join(s->tid, &retval);
	close(s->sd[0]);
	close(s->sd[1]);

	if(retval == NULL)
		return 0;
	return -1;
}

static void *message_thread(void *arg)
{
	struct access_event *event;
	char *filename;
	int rc;
	int dlen;
	struct server *s = arg;

	dlen = strlen(s->cwd);
	if(dlen >= (signed)sizeof(s->cwd) - 2) {
		fprintf(stderr, "Error: CWD[%s] is too large.\n", s->cwd);
		return (void*)-1;
	}
	s->cwd[dlen] = '/';
	s->cwd[dlen+1] = 0;

	event = (struct access_event*)s->msgbuf;
	filename = &s->msgbuf[sizeof(*event)];
	while((rc = recv(s->sd[0], s->msgbuf, sizeof(s->msgbuf), 0)) > 0) {
		int len;
		int expected;

		if(event->at == ACCESS_STOP_SERVER)
			break;
		if(!event->len)
			continue;

		expected = sizeof(*event) + event->len;
		if(rc != expected) {
			fprintf(stderr, "Error: received %i bytes, expecting %i bytes.\n", rc, expected);
			return (void*)-1;
		}

		if(filename[0] == '/') {
			len = canonicalize(filename, s->cname, sizeof(s->cname), NULL);
		} else {
			len = canonicalize2(s->cwd, filename, s->cname, sizeof(s->cname), NULL, "");
		}
		/* Skip the file if it's outside of our local tree */
		if(len < 0)
			continue;

		if(handle_file(event, s->cname, &s->finfo) < 0) {
			return (void*)-1;
		}
	}
	if(rc < 0) {
		perror("recv");
		return (void*)-1;
	}
	return NULL;
}
