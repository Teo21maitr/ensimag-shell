/*****************************************************
 * Copyright Grégory Mounié 2008-2015                *
 *           Simon Nieuviarts 2002-2009              *
 * This code is distributed under the GLPv3 licence. *
 * Ce code est distribué sous la licence GPLv3+.     *
 *****************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <wordexp.h>
#include <sys/resource.h>

#include "variante.h"
#include "readcmd.h"

#ifndef VARIANTE
#error "Variante non défini !!"
#endif

/* Guile (1.8 and 2.0) is auto-detected by cmake */
/* To disable Scheme interpreter (Guile support), comment the
 * following lines.  You may also have to comment related pkg-config
 * lines in CMakeLists.txt.
 */

#define MAX_JOBS 64

typedef struct { pid_t pid; char *cmd; } job_t;
static job_t jobs[MAX_JOBS];
static int njobs = 0;
static int cpu_limit = 0;

static char **expand_cmd(char **cmd) {
	int total = 0;
	wordexp_t we;
	for (int i = 0; cmd[i]; i++) {
		if (wordexp(cmd[i], &we, 0) == 0) {
			total += we.we_wordc;
			wordfree(&we);
		} else {
			total++;
		}
	}
	char **result = malloc((total + 1) * sizeof(char *));
	int k = 0;
	for (int i = 0; cmd[i]; i++) {
		if (wordexp(cmd[i], &we, 0) == 0) {
			for (size_t j = 0; j < we.we_wordc; j++)
				result[k++] = strdup(we.we_wordv[j]);
			wordfree(&we);
		} else {
			result[k++] = strdup(cmd[i]);
		}
	}
	result[k] = NULL;
	return result;
}


static void redir_in(const char *file) {
	int fd = open(file, O_RDONLY);
	if (fd < 0) { perror(file); exit(1); }
	dup2(fd, STDIN_FILENO);
	close(fd);
}

static void redir_out(const char *file) {
	int fd = open(file, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) { perror(file); exit(1); }
	ftruncate(fd, 0);
	dup2(fd, STDOUT_FILENO);
	close(fd);
}

#if USE_GUILE == 1
#include <libguile.h>

int question6_executer(char *line)
{
	struct cmdline *l = parsecmd(&line);
	if (!l || l->err || l->seq[0] == NULL)
		return 0;

	char **cmd = l->seq[0];
	pid_t pid = fork();
	if (pid == 0) {
		if (l->in) redir_in(l->in);
		if (l->out) redir_out(l->out);
		execvp(cmd[0], cmd);
		perror(cmd[0]);
		exit(1);
	}
	waitpid(pid, NULL, 0);
	return 0;
}

SCM executer_wrapper(SCM x)
{
        return scm_from_int(question6_executer(scm_to_locale_stringn(x, 0)));
}
#endif

static void add_job(pid_t pid, char *cmd) {
	if (njobs < MAX_JOBS) {
		jobs[njobs].pid = pid;
		jobs[njobs].cmd = strdup(cmd);
		njobs++;
	}
}

static void print_jobs(void) {
	int i = 0;
	while (i < njobs) {
		if (waitpid(jobs[i].pid, NULL, WNOHANG) > 0) {
			free(jobs[i].cmd);
			jobs[i] = jobs[--njobs];
		} else {
			printf("[%d] %s\n", jobs[i].pid, jobs[i].cmd);
			i++;
		}
	}
}

void terminate(char *line) {
#if USE_GNU_READLINE == 1
	/* rl_clear_history() does not exist yet in centOS 6 */
	clear_history();
#endif
	if (line)
	  free(line);
	printf("exit\n");
	exit(0);
}


int main() {
        printf("Variante %d: %s\n", VARIANTE, VARIANTE_STRING);

#if USE_GUILE == 1
        scm_init_guile();
        /* register "executer" function in scheme */
        scm_c_define_gsubr("executer", 1, 0, 0, executer_wrapper);
#endif

	while (1) {
		struct cmdline *l;
		char *line=0;
		char *prompt = "ensishell>";

		/* Readline use some internal memory structure that
		   can not be cleaned at the end of the program. Thus
		   one memory leak per command seems unavoidable yet */
		line = readline(prompt);
		if (line == 0 || ! strncmp(line,"exit", 4)) {
			terminate(line);
		}

#if USE_GNU_READLINE == 1
		add_history(line);
#endif


#if USE_GUILE == 1
		/* The line is a scheme command */
		if (line[0] == '(') {
			char catchligne[strlen(line) + 256];
			sprintf(catchligne, "(catch #t (lambda () %s) (lambda (key . parameters) (display \"mauvaise expression/bug en scheme\n\")))", line);
			scm_eval_string(scm_from_locale_string(catchligne));
			free(line);
                        continue;
                }
#endif

		/* parsecmd free line and set it up to 0 */
		l = parsecmd( & line);

		/* If input stream closed, normal termination */
		if (!l) {
		  
			terminate(0);
		}
		

		
		if (l->err) {
			/* Syntax error, read another command */
			printf("error: %s\n", l->err);
			continue;
		}

		if (l->seq[0] == NULL)
			continue;

		char **cmd = l->seq[0];

		if (!strcmp(cmd[0], "jobs")) {
			print_jobs();
			continue;
		}

		if (!strcmp(cmd[0], "ulimit") && cmd[1]) {
			cpu_limit = atoi(cmd[1]);
			continue;
		}

		if (l->seq[1] != NULL) {
			int fd[2];
			pipe(fd);

			pid_t pid1 = fork();
			if (pid1 == 0) {
				if (cpu_limit > 0) {
					struct rlimit rl = { cpu_limit, cpu_limit + 5 };
					setrlimit(RLIMIT_CPU, &rl);
				}
				if (l->in) redir_in(l->in);
				dup2(fd[1], STDOUT_FILENO);
				close(fd[0]); close(fd[1]);
				char **ecmd = expand_cmd(l->seq[0]);
				execvp(ecmd[0], ecmd);
				perror(ecmd[0]); exit(1);
			}

			pid_t pid2 = fork();
			if (pid2 == 0) {
				if (cpu_limit > 0) {
					struct rlimit rl = { cpu_limit, cpu_limit + 5 };
					setrlimit(RLIMIT_CPU, &rl);
				}
				dup2(fd[0], STDIN_FILENO);
				close(fd[0]); close(fd[1]);
				if (l->out) redir_out(l->out);
				char **ecmd = expand_cmd(l->seq[1]);
				execvp(ecmd[0], ecmd);
				perror(ecmd[0]); exit(1);
			}

			close(fd[0]); close(fd[1]);
			if (l->bg) {
				add_job(pid1, l->seq[0][0]);
				add_job(pid2, l->seq[1][0]);
			} else {
				waitpid(pid1, NULL, 0);
				waitpid(pid2, NULL, 0);
			}
		} else {
			pid_t pid = fork();
			if (pid == 0) {
				if (cpu_limit > 0) {
					struct rlimit rl = { cpu_limit, cpu_limit + 5 };
					setrlimit(RLIMIT_CPU, &rl);
				}
				if (l->in) redir_in(l->in);
				if (l->out) redir_out(l->out);
				char **ecmd = expand_cmd(cmd);
				execvp(ecmd[0], ecmd);
				perror(ecmd[0]);
				exit(1);
			}
			if (l->bg)
				add_job(pid, cmd[0]);
			else
				waitpid(pid, NULL, 0);
		}
	}

}
