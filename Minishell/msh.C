/*
 * mini-shell (msh)
 * Linux Internals – Educational implementation
 * Features:
 *  - Custom prompt via PS1
 *  - External commands (fork/exec/wait)
 *  - Builtins: exit, cd, pwd, jobs, fg, bg
 *  - Special variables: $?, $$, $SHELL
 *  - Signal handling: Ctrl-C, Ctrl-Z
 *  - Background jobs & job control
 *  - Pipes (multiple)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_CMD 1024
#define MAX_ARGS 100
#define MAX_JOBS 100

/* ---------------- Job structure ---------------- */
typedef struct
{
    pid_t pid;
    char cmd[MAX_CMD];
    int stopped;
} job_t;

job_t jobs[MAX_JOBS];
int job_count = 0;

pid_t fg_pid = -1;
int last_status = 0;
char shell_path[MAX_CMD];

/* ---------------- Utility ---------------- */
void add_job(pid_t pid, const char *cmd, int stopped)
{
    jobs[job_count].pid = pid;
    jobs[job_count].stopped = stopped;
    strncpy(jobs[job_count].cmd, cmd, MAX_CMD);
    job_count++;
}

void remove_job(pid_t pid)
{
    for (int i = 0; i < job_count; i++)
    {
        if (jobs[i].pid == pid)
        {
            for (int j = i; j < job_count - 1; j++)
                jobs[j] = jobs[j + 1];
            job_count--;
            return;
        }
    }
}

job_t *last_job()
{
    if (job_count == 0)
        return NULL;
    return &jobs[job_count - 1];
}

/* ---------------- Signal handlers ---------------- */
void sigint_handler(int sig)
{
    if (fg_pid > 0)
        kill(fg_pid, SIGINT);
    else
        write(1, "\n", 1);
}

void sigtstp_handler(int sig)
{
    if (fg_pid > 0)
    {
        kill(fg_pid, SIGTSTP);
        add_job(fg_pid, "stopped", 1);
        printf("\n[%d] Stopped\n", fg_pid);
        fg_pid = -1;
    }
}

void sigchld_handler(int sig)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        if (WIFEXITED(status))
        {
            printf("\n[%d] Done (exit=%d)\n", pid, WEXITSTATUS(status));
            last_status = WEXITSTATUS(status);
        }
        remove_job(pid);
    }
}

/* ---------------- Parsing ---------------- */
int parse_cmd(char *cmd, char **args)
{
    int i = 0;
    char *token = strtok(cmd, " \t\n");
    while (token)
    {
        args[i++] = token;
        token = strtok(NULL, " \t\n");
    }
    args[i] = NULL;
    return i;
}

/* ---------------- Builtins ---------------- */
int handle_builtin(char **args)
{
    if (!args[0])
        return 1;

    if (strcmp(args[0], "exit") == 0)
        exit(0);

    if (strcmp(args[0], "cd") == 0)
    {
        if (args[1] && chdir(args[1]) != 0)
            perror("cd");
        return 1;
    }

    if (strcmp(args[0], "pwd") == 0)
    {
        char cwd[1024];
        getcwd(cwd, sizeof(cwd));
        printf("%s\n", cwd);
        return 1;
    }

    if (strcmp(args[0], "jobs") == 0)
    {
        for (int i = 0; i < job_count; i++)
            printf("[%d] %s %s\n", jobs[i].pid,
                   jobs[i].stopped ? "Stopped" : "Running",
                   jobs[i].cmd);
        return 1;
    }

    if (strcmp(args[0], "bg") == 0)
    {
        job_t *j = last_job();
        if (j)
        {
            kill(j->pid, SIGCONT);
            j->stopped = 0;
        }
        return 1;
    }

    if (strcmp(args[0], "fg") == 0)
    {
        job_t *j = last_job();
        if (j)
        {
            fg_pid = j->pid;
            kill(j->pid, SIGCONT);
            waitpid(j->pid, &last_status, 0);
            fg_pid = -1;
            remove_job(j->pid);
        }
        return 1;
    }

    if (strcmp(args[0], "echo") == 0 && args[1])
    {
        if (strcmp(args[1], "$?") == 0)
            printf("%d\n", last_status);
        else if (strcmp(args[1], "$$") == 0)
            printf("%d\n", getpid());
        else if (strcmp(args[1], "$SHELL") == 0)
            printf("%s\n", shell_path);
        return 1;
    }

    return 0;
}

/* ---------------- Pipes ---------------- */
void execute_pipes(char *line)
{
    char *cmds[10];
    int n = 0;
    cmds[n++] = strtok(line, "|");
    while ((cmds[n++] = strtok(NULL, "|")));

    int fd[2], in = 0;
    pid_t pid;

    for (int i = 0; i < n - 1; i++)
    {
        pipe(fd);
        pid = fork();
        if (pid == 0)
        {
            dup2(in, 0);
            dup2(fd[1], 1);
            close(fd[0]);
            char *args[MAX_ARGS];
            parse_cmd(cmds[i], args);
            execvp(args[0], args);
            perror("exec");
            exit(1);
        }
        close(fd[1]);
        in = fd[0];
    }
    wait(NULL);
}

/* ---------------- Main ---------------- */
int main(int argc, char *argv[])
{
    realpath(argv[0], shell_path);

    signal(SIGINT, sigint_handler);
    signal(SIGTSTP, sigtstp_handler);
    signal(SIGCHLD, sigchld_handler);

    char line[MAX_CMD];

    while (1)
    {
        char *ps1 = getenv("PS1");
        printf("%s", ps1 ? ps1 : "msh> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        if (strchr(line, '|'))
        {
            execute_pipes(line);
            continue;
        }

        char *args[MAX_ARGS];
        char cmd_copy[MAX_CMD];
        strcpy(cmd_copy, line);
        int argcnt = parse_cmd(line, args);
        if (argcnt == 0)
            continue;

        int bg = 0;
        if (strcmp(args[argcnt - 1], "&") == 0)
        {
            bg = 1;
            args[argcnt - 1] = NULL;
        }

        if (handle_builtin(args))
            continue;

        pid_t pid = fork();
        if (pid == 0)
        {
            execvp(args[0], args);
            perror("exec");
            exit(1);
        }
        else
        {
            if (bg)
            {
                add_job(pid, cmd_copy, 0);
                printf("[%d] Running\n", pid);
            }
            else
            {
                fg_pid = pid;
                waitpid(pid, &last_status, WUNTRACED);
                fg_pid = -1;
            }
        }
    }
    return 0;
}
