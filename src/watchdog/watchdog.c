/*
 * $Header$
 *
 * Handles watchdog connection, and protocol communication with pgpool-II
 *
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2012	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <ctype.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#include "pool.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "pool_config.h"
#include "watchdog/watchdog.h"
#include "watchdog/wd_ext.h"

WdInfo * WD_List = NULL;					/* watchdog server list */
unsigned char * WD_Node_List = NULL;		/* node list */

pid_t wd_ppid = 0;
static pid_t lifecheck_pid;
static pid_t child_pid;
static pid_t hb_receiver_pid[WD_MAX_IF_NUM];
static pid_t hb_sender_pid[WD_MAX_IF_NUM];

static pid_t fork_a_lifecheck(int fork_wait_time);
static void wd_exit(int exit_status);
static void wd_check_config(void);
static int has_setuid_bit(char * path);
static void *exec_func(void *arg);

static void
wd_exit(int exit_signo)
{
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	wd_notice_server_down();

	exit(0);
}

/* send signal specified by sig to watchdog processes */
void
wd_kill_watchdog(int sig)
{
	int i;
    if(lifecheck_pid > 0)
        kill (lifecheck_pid, sig);
    lifecheck_pid = 0;
    
    if(child_pid > 0)
        kill (child_pid, sig);
    child_pid = 0;

	if (!strcmp(pool_config->wd_lifecheck_method, MODE_HEARTBEAT))
	{
		for (i = 0; i < pool_config->num_hb_if; i++)
		{
            if(hb_receiver_pid[i] > 0)
                kill(hb_receiver_pid[i], sig);
            hb_receiver_pid[i] = 0;
            
            if(hb_sender_pid[i] > 0)
                kill (hb_sender_pid[i], sig);
		}
	}
}

static void
wd_check_config(void)
{
	if (pool_config->other_wd->num_wd == 0)
		ereport(ERROR,
			(errmsg("invalid watchdog configuration. other pgpools setting is not defined")));

	if (strlen(pool_config->wd_authkey) > MAX_PASSWORD_SIZE)
		ereport(ERROR,
				(errmsg("invalid watchdog configuration. wd_authkey length can't be larger than %d",
						MAX_PASSWORD_SIZE)));
}

pid_t
wd_main(int fork_wait_time)
{
	int i;

	if (!pool_config->use_watchdog)
		return 0;

	/* check pool_config data */
	wd_check_config();

	/* initialize */
	wd_init();

	wd_ppid = getpid();

	/* launch child process */
	child_pid = wd_child(1);

	if (!strcmp(pool_config->wd_lifecheck_method, MODE_HEARTBEAT))
	{
		for (i = 0; i < pool_config->num_hb_if; i++)
		{
			/* heartbeat receiver process */
			hb_receiver_pid[i] = wd_hb_receiver(1, &(pool_config->hb_if[i]));

			/* heartbeat sender process */
			hb_sender_pid[i] = wd_hb_sender(1, &(pool_config->hb_if[i]));
		}
	}

	/* fork lifecheck process*/
	lifecheck_pid = fork_a_lifecheck(fork_wait_time);

	return lifecheck_pid;
}


/* fork lifecheck process*/
static pid_t
fork_a_lifecheck(int fork_wait_time)
{
	pid_t pid;
	sigjmp_buf	local_sigjmp_buf;

	pid = fork();
	if (pid != 0)
	{
		if (pid == -1)
			ereport(ERROR,
					(errmsg("failed to fork a lifecheck process")));
		return pid;
	}
    on_exit_reset();
	processType = PT_LIFECHECK;

	if (fork_wait_time > 0) {
		sleep(fork_wait_time);
	}

	POOL_SETMASK(&UnBlockSig);

	init_ps_display("", "", "", "");

	signal(SIGTERM, wd_exit);	
	signal(SIGINT, wd_exit);	
	signal(SIGQUIT, wd_exit);	
	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_IGN);	
	signal(SIGPIPE, SIG_IGN);

	/* Create per loop iteration memory context */
	ProcessLoopContext = AllocSetContextCreate(TopMemoryContext,
											   "wd_lifecheck_main_loop",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
	
	MemoryContextSwitchTo(TopMemoryContext);

	set_ps_display("lifecheck",false);

	/* wait until ready to go */
	while (WD_OK != is_wd_lifecheck_ready())
	{
		sleep(pool_config->wd_interval * 10);
	}
	ereport(LOG,
			(errmsg("watchdog: lifecheck started")));

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;
		
		EmitErrorReport();
		MemoryContextSwitchTo(TopMemoryContext);
		FlushErrorState();
		sleep(pool_config->wd_heartbeat_keepalive);
	}
	
	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/* watchdog loop */
	for (;;)
	{
		MemoryContextSwitchTo(ProcessLoopContext);
		MemoryContextResetAndDeleteChildren(ProcessLoopContext);

		/* pgpool life check */
		wd_lifecheck();
		sleep(pool_config->wd_interval);
	}

	return pid;
}

/* if pid is for one of watchdog processes return 1, othewize return 0 */
int
wd_is_watchdog_pid(pid_t pid)
{
	int i;

	if (pid == lifecheck_pid || pid == child_pid)
	{
		return 1;
	}

	for (i = 0; i < pool_config->num_hb_if; i++)
	{
		if (pid == hb_receiver_pid[i] || pid == hb_sender_pid[i])
		{
			return 1;
		}
	}

	return 0;
}

/* restart watchdog process specified by pid */
int
wd_reaper_watchdog(pid_t pid, int status)
{
	int i;

	/* watchdog lifecheck process exits */
	if (pid == lifecheck_pid)
	{
		if (WIFSIGNALED(status))
			ereport(DEBUG1,
					(errmsg("watchdog lifecheck process with PID:%d exits with status %d by signal %d",
							pid, status, WTERMSIG(status))));
		else
			ereport(DEBUG1,
					(errmsg("watchdog lifecheck process with PID:%d exits with status %d", pid, status)));

		lifecheck_pid = fork_a_lifecheck(1);

		ereport(LOG,
				(errmsg("fork a new watchdog lifecheck pid %d", lifecheck_pid)));

	}

	/* watchdog child process exits */
	else if (pid == child_pid)
	{
		if (WIFSIGNALED(status))
			ereport(DEBUG1,
					(errmsg("watchdog child process with PID:%d exits with status %d by signal %d",
							pid, status,WTERMSIG(status))));
		else
			ereport(DEBUG1,
				(errmsg("watchdog child process with PID:%d exits with status %d",
						pid, status)));


		child_pid = wd_child(1);

		ereport(LOG,
				(errmsg("fork a new watchdog child pid %d", child_pid)));

	}

	/* receiver/sender process exits */
	else
	{
		for (i = 0; i < pool_config->num_hb_if; i++)
		{
			if (pid == hb_receiver_pid[i])
			{
				if (WIFSIGNALED(status))
					ereport(DEBUG1,
							(errmsg("watchdog heartbeat receiver process with PID:%d exits with status %d by signal %d",
									pid, status, WTERMSIG(status))));
				else
					ereport(DEBUG1,
							(errmsg("watchdog heartbeat receiver process with PID:%d exits with status %d", pid, status)));

				hb_receiver_pid[i] = wd_hb_receiver(1, &(pool_config->hb_if[i]));

				ereport(LOG,
						(errmsg("fork a new watchdog heartbeat receiver with pid %d", hb_receiver_pid[i])));

				break;
			}

			else if (pid == hb_sender_pid[i])
			{
				if (WIFSIGNALED(status))
					ereport(DEBUG1,
							(errmsg("watchdog heartbeat sender process with PID:%d exits with status %d by signal %d",
									pid, status, WTERMSIG(status))));
				else
					ereport(DEBUG1,
							(errmsg("watchdog heartbeat sender process with PID:%d exits with status %d", pid, status)));

				hb_sender_pid[i] = wd_hb_sender(1, &(pool_config->hb_if[i]));

				ereport(LOG,
						(errmsg("fork a new watchdog heartbeat sender with PID:%d", hb_sender_pid[i])));
				break;
			}
		}
	}
	
	return 1;
}

int
wd_chk_setuid(void)
{
	char path[128];
	char cmd[128];
	
	/* check setuid bit of ifup command */
	wd_get_cmd(cmd, pool_config->if_up_cmd);
	snprintf(path, sizeof(path), "%s/%s", pool_config->ifconfig_path, cmd);
	if (! has_setuid_bit(path))
	{
		ereport(NOTICE,
			(errmsg("checking setuid bit of ifup command"),
				 errdetail("ifup[%s] doesn't have setuid bit", path)));
		return 0;
	}

	/* check setuid bit of ifdown command */
	wd_get_cmd(cmd, pool_config->if_down_cmd);
	snprintf(path, sizeof(path), "%s/%s", pool_config->ifconfig_path, cmd);
	if (! has_setuid_bit(path))
	{
		ereport(NOTICE,
			(errmsg("checking setuid bit of ifdown command"),
				 errdetail("ifdown[%s] doesn't have setuid bit", path)));
		return 0;
	}

	/* check setuid bit of arping command */
	wd_get_cmd(cmd, pool_config->arping_cmd);
	snprintf(path, sizeof(path), "%s/%s", pool_config->arping_path, cmd);
	if (! has_setuid_bit(path))
	{
		ereport(NOTICE,
			(errmsg("checking setuid bit of arping command"),
				 errdetail("arping[%s] doesn't have setuid bit", path)));

		return 0;
	}
	ereport(NOTICE,
		(errmsg("checking setuid bit of required commands"),
			 errdetail("all commands have proper setuid bit")));
	return 1;
}

/* 
 * if the file has setuid bit and the owner is root, it returns 1, otherwise returns 0 
 */
static int
has_setuid_bit(char * path)
{
	struct stat buf;
	if (stat(path,&buf) < 0)
	{
		ereport(FATAL,
			(return_code(1),
			 errmsg("has_setuid_bit: command '%s' not found", path)));
	}
	return ((buf.st_uid == 0) && (S_ISREG(buf.st_mode)) && (buf.st_mode & S_ISUID))?1:0;
}


/*
 * The function is wrapper over pthread_create and calls the user provided
 * thread function inside PG_TRY, CATCH block to make sure any ereport ERROR
 * from thread should stay within the thread.
 */
int watchdog_thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
	WdThreadInfo* thread_arg = palloc(sizeof(WdThreadInfo));
	thread_arg->arg = arg;
	thread_arg->start_routine = start_routine;
	return pthread_create(thread, attr, exec_func, thread_arg);
}

static void *
exec_func(void *arg)
{
	unsigned long rtn = (unsigned long)WD_NG;
	MemoryContext oldContext =  CurrentMemoryContext;
	WdThreadInfo* thread_arg = (WdThreadInfo*) arg;
	Assert(thread_arg != NULL);
	
	PG_TRY();
	{
		rtn = thread_arg->start_routine(thread_arg->arg);
	}
	PG_CATCH();
	{
		/* ignore the error message */
		EmitErrorReport();
		MemoryContextSwitchTo(oldContext);
		FlushErrorState();
	}
	PG_END_TRY();
	return rtn;
}