/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "tntps.h"
#include "trivia/config.h"

#include <assert.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <say.h>

extern "C" {
void set_proc_title(const char *format, ...);
} /* extern "C" */

/**
 * See tntps.h for the description of the main and anchor processes.
 * Tarantool forks early. Process tree (assume Tarantool launched from
 * sh):
 *
 *  sh
 *  \_anchor process (Tarantool)
 *     \_main process (Tarantool)
 *
 * Anchor and main processes are connected with a pipe. It allows main
 * process to send commands (simple IPC). Anchor process waits for the
 * main process termination and exits. To implement the background mode
 * the main process commands the anchor to exit early (via IPC).
 *
 * In foreground mode Tarantool instance consists of the two processes
 * (anchor + main). If a process unexpectedly dies we would like another
 * one to notice and exit as well. Anchor process is already monitoring
 * the main process.
 *
 * If the anchor process itself dies (say, someone does kill -9), the
 * IPC pipe read end is closed, and the main process observes the write
 * end state changed. We could have done pipe monitoring using the event
 * loop; in order to make it more robust a signal-based solution was
 * implemented instead. We arrange for the system to signal the main
 * process whenever the pipe state changes (O_ASYNC, SIGIO) and we check
 * the pipe in a signal handler.
 */

static int tntps_ipc_fd = -1;
static pid_t tntps_main_pid = -1;

enum {
	IPC_PIPE_CLOSED = 0,

	/* Request anchor process termination. */
	IPC_EXIT = 1,

	/* Request anchor process to update its title. */
	IPC_SET_PROC_TITLE = 2,

	/* Fiber stack currently 64K, hence a relatively low limit. */
	IPC_PAYLOAD_MAX = (1024 - 8)
};

struct ipc_message
{
	uint32_t code;
	uint32_t payload_len;
	uint8_t  payload[IPC_PAYLOAD_MAX];
};

/**
 * Write a message to IPC pipe, returns 0 on success, otherwise -1
 */
static int
tntps_ipc_write(int fd, uint32_t code, const void *payload, size_t payload_len)
{
	struct ipc_message msg;
	const uint8_t *bytes;
	size_t bytes_remaining;

	if (payload_len > IPC_PAYLOAD_MAX) {
		errno = E2BIG;
		return -1;
	}

	msg.code = code;
	msg.payload_len = (uint32_t)payload_len;
	memcpy(msg.payload, payload, payload_len);

	bytes = (const uint8_t *)&msg;
	bytes_remaining = offsetof(struct ipc_message, payload) + payload_len;

	while (bytes_remaining != 0) {
		/*
		 * SIGPIPE if the pipe unexpectedly closed is fine - we are
		 * killing the main process anyway
		 */
		ssize_t bytes_sent = write(fd, bytes, bytes_remaining);

		if (bytes_sent == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		bytes += bytes_sent;
		bytes_remaining -= bytes_sent;
	}

	return 0;
}

/**
 * Read a message from IPC pipe, returns 0 on success, otherwise -1
 * On EOF returns 0 and sets msg->code to IPC_PIPE_CLOSED.
 */
static int
tntps_ipc_read(int fd, struct ipc_message* msg)
{
	assert(msg);

	const size_t header_len = offsetof(struct ipc_message, payload);
	uint8_t *bytes = (uint8_t *)msg;
	size_t bytes_remaining = header_len;

	while (1) {
		ssize_t bytes_read;
		if (bytes_remaining == 0) {
			if (bytes == msg->payload) {
				/* header ready, probably payload follows */
				if (msg->payload_len == 0)
					return 0;
				if (msg->payload_len > IPC_PAYLOAD_MAX) {
					assert(0);
					errno = EINVAL;
					return -1;
				}
				bytes_remaining += msg->payload_len;
				continue;
			}
			break;
		}
		bytes_read = read(fd, bytes, bytes_remaining);
		if (bytes_read == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (bytes_read == 0) {
			msg->code = IPC_PIPE_CLOSED;
			msg->payload_len = 0;
			return 0;
		}
		bytes += bytes_read;
		bytes_remaining -= bytes_read;
	}
	return 0;
}

/**
 * Relay select signals into the main process.
 * Executed in the anchor process.
 */
static void
tntps_relay_signal(int sig)
{
	assert(tntps_main_pid != (pid_t)-1);
	kill(tntps_main_pid, sig);
}

/**
 * Kill main process if IPC pipe unexpectedly disconnected.
 * Executed in the main process.
 */
static void
tntps_maybe_kill(int sig)
{
	assert(tntps_ipc_fd != -1);

	int prev_errno;
	struct pollfd pf;

	(void)sig;

	prev_errno = errno;

	/*
	 * Check if the IPC pipe actually disconnected.
	 * SIGIO could be due to us writing to the IPC pipe or the peer
	 * reading from it.
	 */
	pf.fd = tntps_ipc_fd;
	pf.events = POLLOUT|POLLIN;
	pf.revents = 0;

	if (poll(&pf, 1, 0) == 1 && (pf.revents & (POLLHUP|POLLERR)))
		kill(getpid(), SIGKILL);

	errno = prev_errno;
}

void tntps_init_main_process()
{
	assert(tntps_main_pid == (pid_t)-1);

	enum {
		PIPE_READ_END = 0,
		PIPE_WRITE_END = 1
	};

	int exit_status = EXIT_SUCCESS;
	int pipe_fd[2];
	pid_t child_pid;
	int wait_status;

	if (pipe(pipe_fd) == -1)
		panic_syserror("pipe");

	switch ((child_pid = fork())) {
	case -1:
		panic_syserror("fork");
	case 0:
		/*
		 * Main process branch.
		 */
		if (close(pipe_fd[PIPE_READ_END]) == -1)
			panic_syserror("close");
		/*
		 * Arrange for the child to get killed if the parent
		 * unexpectedly exits.
		 */
		if (fcntl(pipe_fd[PIPE_WRITE_END], F_SETFD, O_CLOEXEC) == -1 ||
			fcntl(pipe_fd[PIPE_WRITE_END], F_SETFL, O_ASYNC) == -1 ||
			fcntl(pipe_fd[PIPE_WRITE_END], F_SETOWN, getpid()) == -1)
			say_syserror("fcntl");
		if (signal(SIGIO, tntps_maybe_kill) == SIG_ERR)
			say_syserror("signal");
		tntps_ipc_fd = pipe_fd[PIPE_WRITE_END];
		return;
	}

	/*
	 * Anchor process branch.
	 */
	tntps_main_pid = child_pid;

	if (close(pipe_fd[PIPE_WRITE_END]) == -1)
		panic_syserror("close");

	/*
	 * Relay select signals.
	 * Ex: user starts Tarantool in foreground mode and records the pid;
	 *     now she wants to stop it so she kills the pid with SIGTERM.
	 */
	if (signal(SIGTERM, tntps_relay_signal) == SIG_ERR ||
		signal(SIGHUP,  tntps_relay_signal) == SIG_ERR ||
		signal(SIGINT,  tntps_relay_signal) == SIG_ERR ||
		signal(SIGUSR1, tntps_relay_signal) == SIG_ERR)
		say_syserror("signal");

	while (1) {
		struct ipc_message msg;

		if (tntps_ipc_read(pipe_fd[PIPE_READ_END], &msg) == -1)
			continue;

		switch (msg.code) {

		case IPC_PIPE_CLOSED:
			/* Main process exited w/o telling us, propagate status. */
			if (waitpid(child_pid, &wait_status, 0) == (pid_t)-1)
				panic_syserror("waitpid");

			exit_status = WIFEXITED(wait_status) ?
				WEXITSTATUS(wait_status) : EXIT_FAILURE;

			if (WIFSIGNALED(wait_status))
				say(S_SYSERROR, NULL, "%s%s", strsignal(WTERMSIG(wait_status)),
					WCOREDUMP(wait_status) ? " (core dumped)" : "");

			exit(exit_status);

		case IPC_EXIT:
			/* Master process asks us to exit. */
			exit(EXIT_SUCCESS);

		case IPC_SET_PROC_TITLE:
			/* The string is lacking \0 termination. */
			set_proc_title("%.*s",
			               (int)msg.payload_len, (const char*)msg.payload);
			break;
		}
	}
}

void tntps_enter_background_mode()
{
	assert(tntps_ipc_fd != -1);

	if (signal(SIGIO, SIG_DFL) == SIG_ERR)
		panic_syserror("signal");

	if (tntps_ipc_write(tntps_ipc_fd, IPC_EXIT, NULL, 0) == -1)
		panic_syserror("write");

	if (close(tntps_ipc_fd) == -1)
		say_syserror("close");

	tntps_ipc_fd = -1;
}

void tntps_on_proc_title_changed(const char *new_title)
{
	assert(new_title);

	size_t len;

	if (tntps_ipc_fd == -1)
		return;

	len = strlen(new_title);
	if (len > IPC_PAYLOAD_MAX)
		len = IPC_PAYLOAD_MAX;
	tntps_ipc_write(tntps_ipc_fd,
	                IPC_SET_PROC_TITLE, new_title, len);
}
