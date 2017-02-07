/******************************************************************************
  Copyright 2011 Todd Sundsted. All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY TODD SUNDSTED ``AS IS'' AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
  EVENT SHALL TODD SUNDSTED OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
  OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  The views and conclusions contained in the software and documentation are
  those of the authors and should not be interpreted as representing official
  policies, either expressed or implied, of Todd Sundsted.
 *****************************************************************************/

#include <errno.h>
#include <sys/stat.h>

#include "my-fcntl.h"
#include "my-signal.h"
#include "my-stdlib.h"
#include "my-string.h"
#include "my-unistd.h"

#include "net_multi.h"

#include "exec.h"
#include "functions.h"
#include "list.h"
#include "log.h"
#include "storage.h"
#include "structures.h"
#include "streams.h"
#include "tasks.h"
#include "utils.h"

typedef enum {
    TWS_CONTINUE,		/* The task is running and has not yet
				 * stopped or been killed.
				 */
    TWS_STOP,			/* The task has stopped.  This status
				 * is final.
				 */
    TWS_KILL			/* The task has been killed.  This
				 * status is final.
				 */
} task_waiting_status;

struct task_waiting_on_exec {
    char *cmd;
    int count;
    char **args;
    int length;
    char *in;
    pid_t pid;
    task_waiting_status status;
    int code;
    int fin;
    int fout;
    int ferr;
    Stream *sout;
    Stream *serr;
    vm the_vm;

    task_waiting_on_exec(const char* cmd,
                         int count, const char** args,
                         int length, const char* in) {
	this->cmd = new char[strlen(cmd) + 1];
	strcpy(this->cmd, cmd);
	this->count = count;
	this->args = new char*[count + 1];
	for (int i = 0; i < count + 1; i++) {
	    if (args[i]) {
		this->args[i] = new char[strlen(args[i]) + 1];
		strcpy(this->args[i], args[i]);
	    } else {
		this->args[i] = nullptr;
	    }
	}
	this->length = length;
	this->in = new char[length];
	memcpy(this->in, in, length);
	status = TWS_CONTINUE;
	code = 0;
	sout = new_stream(1000);
	serr = new_stream(1000);
    }

    ~task_waiting_on_exec() {
	delete cmd;
	for (int i = 0; i < count; i++) {
	    if (args[i]) {
		delete args[i];
	    }
	}
	delete args;
	delete in;
	close(fout);
	close(ferr);
	network_unregister_fd(fout);
	network_unregister_fd(ferr);
	free_stream(sout);
	free_stream(serr);
    }
};

static task_waiting_on_exec *process_table[EXEC_MAX_PROCESSES];

volatile static sig_atomic_t sigchild_interrupt = 0;

static sigset_t block_sigchld;

#define BLOCK_SIGCHLD sigprocmask(SIG_BLOCK, &block_sigchld, NULL)
#define UNBLOCK_SIGCHLD sigprocmask(SIG_UNBLOCK, &block_sigchld, NULL)

static task_waiting_on_exec*
malloc_task_waiting_on_exec(const char* cmd, int count, const char** args, int length, const char* in)
{
    return new task_waiting_on_exec(cmd, count, args, length, in);
}

static void
free_task_waiting_on_exec(task_waiting_on_exec* tw)
{
    delete tw;
}

static task_enum_action
exec_waiter_enumerator(task_closure closure, void *data)
{
    task_enum_action action = TEA_CONTINUE;

    BLOCK_SIGCHLD;

    int i;
    for (i = 0; i < EXEC_MAX_PROCESSES; i++) {
	if (process_table[i]) {
	    if (TWS_KILL != process_table[i]->status) {
		action = (*closure) (process_table[i]->the_vm,
				     process_table[i]->cmd,
				     data);
		if (TEA_KILL == action)
		    process_table[i]->status = TWS_KILL;
		if (TEA_CONTINUE != action)
		    break;
	    }
	}
    }

    UNBLOCK_SIGCHLD;

    return action;
}

static int
write_all(int fd, const char *buffer, size_t length)
{
    ssize_t count;
    while (length) {
	if ((count = write(fd, buffer, length)) < 0)
	    return -1;
	buffer += count;
	length -= count;
    }
    fsync(fd);
    return 1;
}

static void
stdout_readable(int fd, void *data)
{
    task_waiting_on_exec *tw = (task_waiting_on_exec *)data;
    char buffer[1000];
    int n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
	stream_add_string(tw->sout, raw_bytes_to_binary(buffer, n));
    }
}

static void
stderr_readable(int fd, void *data)
{
    task_waiting_on_exec *tw = (task_waiting_on_exec *)data;
    char buffer[1000];
    int n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
	stream_add_string(tw->serr, raw_bytes_to_binary(buffer, n));
    }
}

static pid_t
fork_and_exec(const char *cmd, const char *const args[], const char *const env[],
	      int *in, int *out, int *err)
{
    pid_t pid;
    int pipeIn[2];
    int pipeOut[2];
    int pipeErr[2];

    if (pipe(pipeIn) < 0) {
	log_perror("EXEC: Couldn't create pipe - in");
	goto fail;
    }
    else if (pipe(pipeOut) < 0) {
	log_perror("EXEC: Couldn't create pipe - out");
	goto close_in;
    }
    else if (pipe(pipeErr) < 0) {
	log_perror("EXEC: Couldn't create pipe - err");
	goto close_out;
    }
    else if ((pid = fork()) < 0) {
	log_perror("EXEC: Couldn't fork");
	goto close_err;
    }
    else if (0 == pid) { /* child */
	int status;

	if ((status = dup2(pipeIn[0], STDIN_FILENO)) < 0) {
	    perror("dup2");
	    exit(status);
	}
	if ((status = dup2(pipeOut[1], STDOUT_FILENO)) < 0) {
	    perror("dup2");
	    exit(status);
	}
	if ((status = dup2(pipeErr[1], STDERR_FILENO)) < 0) {
	    perror("dup2");
	    exit(status);
	}

	close(pipeIn[1]);
	close(pipeOut[0]);
	close(pipeErr[0]);

	status = execve(cmd, (char *const *)args, (char *const *)env);
	perror("execve");
	exit(status);
    }

    close(pipeIn[0]);
    close(pipeOut[1]);
    close(pipeErr[1]);

    *in = pipeIn[1];
    *out = pipeOut[0];
    *err = pipeErr[0];

    return pid;

 close_err:
    close(pipeErr[0]);
    close(pipeErr[1]);

 close_out:
    close(pipeOut[0]);
    close(pipeOut[1]);

 close_in:
    close(pipeIn[0]);
    close(pipeIn[1]);

 fail:
    return 0;
}

static int
set_nonblocking(int fd)
{
    int flags;

    if ((flags = fcntl(fd, F_GETFL, 0)) < 0
	|| fcntl(fd, F_SETFL, flags | NONBLOCK_FLAG) < 0)
	return 0;
    else
	return 1;
}

static enum error
exec_waiter_suspender(vm the_vm, void *data)
{
    task_waiting_on_exec *tw = (task_waiting_on_exec *)data;
    enum error error = E_QUOTA;

    BLOCK_SIGCHLD;

    int i;
    for (i = 0; i < EXEC_MAX_PROCESSES; i++) {
	if (process_table[i] == NULL) {
	    process_table[i] = tw;
	    break;
	}
    }
    if (i == EXEC_MAX_PROCESSES) {
	error = E_QUOTA;
	goto free_task_waiting_on_exec;
    }

    static const char *env[] = { "PATH=/bin:/usr/bin", NULL };

    if ((tw->pid = fork_and_exec(tw->cmd, tw->args, env, &tw->fin, &tw->fout, &tw->ferr)) == 0) {
	error = E_EXEC;
	goto clear_process_slot;
    }

    oklog("EXEC: %s (%d)...\n", tw->cmd, tw->pid);

    set_nonblocking(tw->fin);
    set_nonblocking(tw->fout);
    set_nonblocking(tw->ferr);

    if (tw->in) {
	if (write_all(tw->fin, tw->in, tw->length) < 0) {
	    error = E_EXEC;
	    goto close_fin;
	}
    }

    close(tw->fin);

    network_register_fd(tw->fout, stdout_readable, NULL, tw);
    network_register_fd(tw->ferr, stderr_readable, NULL, tw);

    tw->the_vm = the_vm;

 /* success */
    UNBLOCK_SIGCHLD;
    return E_NONE;

 close_fin:
    close(tw->fin);

 clear_process_slot:
    process_table[i] = NULL;

 free_task_waiting_on_exec:
    free_task_waiting_on_exec(tw);

 /* fail */
    UNBLOCK_SIGCHLD;
    return error;
}

static package
bf_exec(const List& arglist, Objid progr)
{
    package pack;

    const char* cmd = nullptr;
    int count = listlength(arglist[1]);
    const char* args[count + 1];
    int length = 0;
    const char *in = nullptr;
    static Stream *s = new_stream(strlen(EXEC_SUBDIR) * 2);
    task_waiting_on_exec *tw;

    /* The first argument must be a list of strings.  The first string
     * is the command (required).  The rest are command line arguments
     * to the command.
     */
    Var v;
    int i, c;
    FOR_EACH(v, arglist[1], i, c) {
	if (!v.is_str()) {
	    pack = make_error_pack(E_INVARG);
	    goto free_arglist;
	}
    }
    /* check for the empty list */
    if (1 == i) {
	pack = make_error_pack(E_INVARG);
	goto free_arglist;
    }

    /* check the path */
    cmd = arglist[1].v.list[1].v.str.expose();
    if (0 == strlen(cmd)) {
	pack = make_raise_pack(E_INVARG, "Invalid path", var_ref(zero));
	goto free_arglist;
    }
    if (('/' == cmd[0])
	|| (1 < strlen(cmd) && '.' == cmd[0] && '.' == cmd[1])) {
	pack = make_raise_pack(E_INVARG, "Invalid path", var_ref(zero));
	goto free_arglist;
    }
    if (strstr(cmd, "/.") || strstr(cmd, "./")) {
	pack = make_raise_pack(E_INVARG, "Invalid path", var_ref(zero));
	goto free_arglist;
    }

    /* prepend the exec subdirectory path */
    stream_add_string(s, EXEC_SUBDIR);
    stream_add_string(s, cmd);
    cmd = reset_stream(s);

    /* args must be null terminated */
    FOR_EACH(v, arglist[1], i, c)
	args[i - 1] = v.v.str.expose();
    args[i - 1] = NULL;

    /* clean input */
    if (listlength(arglist) > 1) {
	const char* tmp;
	if ((tmp = binary_to_raw_bytes(arglist[2].v.str.expose(), &length)) == NULL) {
	    pack = make_error_pack(E_INVARG);
	    goto free_arglist;
	}
	in = tmp;
    }

    /* check perms */
    if (!is_wizard(progr)) {
	pack = make_error_pack(E_PERM);
	goto free_arglist;
    }

    /* stat the command */
    struct stat buf;
    if (stat(cmd, &buf) != 0) {
	pack = make_raise_pack(E_INVARG, "Does not exist", var_ref(zero));
	goto free_arglist;
    }
    if (!S_ISREG(buf.st_mode)) {
	pack = make_raise_pack(E_INVARG, "Is not a file", var_ref(zero));
	goto free_arglist;
    }

    tw = malloc_task_waiting_on_exec(cmd, count, args, length, in);

    free_var(arglist);

    return make_suspend_pack(exec_waiter_suspender, tw);

 free_arglist:
    free_var(arglist);

 /* fail */
    return pack;
}

/*
 * Called from child_completed_signal() in server.c.
 * SIGCHLD is already blocked.
 */
pid_t
exec_complete(pid_t pid, int code)
{
    task_waiting_on_exec *tw = NULL;

    int i;
    for (i = 0; i < EXEC_MAX_PROCESSES; i++)
	if (process_table[i] && process_table[i]->pid == pid) {
	    tw = process_table[i];
	    break;
	}

    if (tw) {
	sigchild_interrupt = 1;

	if (TWS_CONTINUE == tw->status) {
	    tw->status = TWS_STOP;
	    tw->code = code;
	}

	return pid;
    }

    /* We wind up here if the child process was a checkpoint process,
     * or if an exec task was explicitly killed while the process
     * itself was still executing.
     */
    return 0;
}

/*
 * Called from main_loop() in server.c.
 */
void
deal_with_child_exit(void)
{
    if (!sigchild_interrupt)
	return;

    BLOCK_SIGCHLD;

    sigchild_interrupt = 0;

    task_waiting_on_exec *tw = NULL;

    int i;
    for (i = 0; i < EXEC_MAX_PROCESSES; i++) {
	tw = process_table[i];
	if (tw && TWS_STOP == tw->status) {
	    List v = new_list(3);
	    v.v.list[1] = Var::new_int(tw->code);
	    stdout_readable(tw->fout, tw);
	    v.v.list[2] = Var::new_str(reset_stream(tw->sout));
	    stderr_readable(tw->ferr, tw);
	    v.v.list[3] = Var::new_str(reset_stream(tw->serr));

	    resume_task(tw->the_vm, v);
	}
	if (tw && TWS_CONTINUE != tw->status) {
	    free_task_waiting_on_exec(tw);
	    process_table[i] = NULL;
	}
    }

    UNBLOCK_SIGCHLD;
}

void
register_exec(void)
{
    sigemptyset(&block_sigchld);
    sigaddset(&block_sigchld, SIGCHLD);

    register_task_queue(exec_waiter_enumerator);
    register_function("exec", 1, 2, bf_exec, TYPE_LIST, TYPE_STR);
}
