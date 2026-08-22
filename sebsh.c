// sebsh, by Sebminecrafter
// sebsh © 2026 by Sebminecrafter is licensed under CC BY-SA 4.0. To view a copy of this license, visit https://creativecommons.org/licenses/by-sa/4.0/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <io.h>
#define GETCWD _getcwd
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#define GETCWD getcwd

// Save so it can restore on exit
static struct termios saved_termios;
static int termios_saved = 0;

static void restore_termios(void)
{
    if (termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

// Suppress ECHOCTL so '^C' doesn't echo on screen (visible on tty/framebuffer/etc.)
static void suppress_echoctl(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) < 0)
        return;
    saved_termios = t;
    termios_saved = 1;
    atexit(restore_termios);
#ifdef ECHOCTL
    t.c_lflag &= ~(tcflag_t)ECHOCTL;
#endif
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

// Ensure sebsh is session leader
static void acquire_controlling_tty(void)
{
    if (!isatty(STDIN_FILENO))
        return;

    if (getsid(0) != getpid())
    {
        if (setsid() < 0)
            return;
    }

#ifdef TIOCSCTTY
    (void)ioctl(STDIN_FILENO, TIOCSCTTY, 0);
#endif

    (void)tcsetpgrp(STDIN_FILENO, getpgrp());
}
#endif

#define VER "1.0.0"
#define CMD_SIZE 4096
#define MAX_ARGS 100
#define MAX_PIPE_SEGMENTS 16

static volatile sig_atomic_t interrupt_requested = 0;

#ifndef _WIN32
static volatile sig_atomic_t foreground_child_pid = 0;
#endif

void handle_interrupt(int signal_number)
{
    (void)signal_number;
    interrupt_requested = 1;
#ifdef _WIN32
    _write(_fileno(stdout), "\n", 1);
#else
    pid_t child = foreground_child_pid;
    if (child > 0)
        kill(-child, SIGINT);
    else
        write(STDOUT_FILENO, "\n", 1);
#endif
}

typedef struct
{
    bool debug;
    bool running;
    char cwd[FILENAME_MAX];
    const char *prompt;
} sebsh;

typedef struct
{
    char *command;
    char *args[MAX_ARGS];
    int arg_count;
    char *redirect_in;     //
    char *redirect_out;    // >
    char *redirect_append; // >>
} CommandInfo;

// Cross-platform process spawn function
int spawn_process(const char *path, char *const argv[], const char *redirect_in, const char *redirect_out, const char *redirect_append)
{
#ifdef _WIN32
    int result = _spawnvp(_P_WAIT, path, argv);
    if (result == -1)
    {
        perror("_spawnvp");
    }
    return result;
#else
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        return -1;
    }
    if (pid == 0)
    {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        if (redirect_in)
        {
            int fd = open(redirect_in, O_RDONLY);
            if (fd < 0)
            {
                perror(redirect_in);
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        if (redirect_append)
        {
            int fd = open(redirect_append, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0)
            {
                perror(redirect_append);
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        else if (redirect_out)
        {
            int fd = open(redirect_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0)
            {
                perror(redirect_out);
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        execvp(path, argv);
        perror("execvp");
        exit(EXIT_FAILURE);
    }
    setpgid(pid, pid);
    foreground_child_pid = pid;
    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, pid);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno != EINTR)
            break;
    }
    foreground_child_pid = 0;
    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, getpgrp());

    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}

int split_pipe(char *input, char *segments[], int max_segments)
{
    int count = 0;
    bool in_single = false, in_double = false;
    char *p = input;
    segments[count++] = p;

    while (*p != '\0' && count < max_segments)
    {
        if (*p == '\'' && !in_double)
            in_single = !in_single;
        else if (*p == '"' && !in_single)
            in_double = !in_double;
        else if (*p == '|' && !in_single && !in_double)
        {
            *p = '\0';
            segments[count++] = p + 1;
        }
        p++;
    }
    segments[count] = NULL;
    return count;
}

#ifndef _WIN32
void execute_pipeline(CommandInfo *infos, int count)
{
    int pipes[MAX_PIPE_SEGMENTS - 1][2];

    for (int i = 0; i < count - 1; i++)
    {
        if (pipe(pipes[i]) < 0)
        {
            perror("pipe");
            return;
        }
    }

    pid_t pipeline_pgid = 0; /* all pipeline children share one pgid */
    pid_t pids[MAX_PIPE_SEGMENTS] = {0};

    for (int i = 0; i < count; i++)
    {
        if (infos[i].command == NULL)
            continue;
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            return;
        }
        if (pid == 0)
        {
            setpgid(0, pipeline_pgid == 0 ? 0 : pipeline_pgid);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);
            else if (infos[i].redirect_in)
            {
                int fd = open(infos[i].redirect_in, O_RDONLY);
                if (fd < 0)
                {
                    perror(infos[i].redirect_in);
                    exit(EXIT_FAILURE);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            if (i < count - 1)
                dup2(pipes[i][1], STDOUT_FILENO);
            else if (infos[i].redirect_append)
            {
                int fd = open(infos[i].redirect_append, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd < 0)
                {
                    perror(infos[i].redirect_append);
                    exit(EXIT_FAILURE);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            else if (infos[i].redirect_out)
            {
                int fd = open(infos[i].redirect_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0)
                {
                    perror(infos[i].redirect_out);
                    exit(EXIT_FAILURE);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            for (int j = 0; j < count - 1; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            execvp(infos[i].command, infos[i].args);
            perror("execvp");
            exit(EXIT_FAILURE);
        }
        if (pipeline_pgid == 0)
        {
            pipeline_pgid = pid;
            setpgid(pid, pid);
            foreground_child_pid = pipeline_pgid;
            if (isatty(STDIN_FILENO))
                tcsetpgrp(STDIN_FILENO, pipeline_pgid);
        }
        else
        {
            setpgid(pid, pipeline_pgid);
        }
        pids[i] = pid;
    }

    for (int i = 0; i < count - 1; i++)
    {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    for (int i = 0; i < count; i++)
    {
        if (pids[i] == 0)
            continue;
        int status;
        while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR)
            continue;
    }
    foreground_child_pid = 0;
    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, getpgrp());
}
#endif

char *trim(char *str)
{
    if (str == NULL)
        return NULL;
    char *end;
    while (isspace((unsigned char)*str))
        str++;
    if (*str == '\0')
        return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;
    end[1] = '\0';
    return str;
}

CommandInfo parse_input(char *input_str)
{
    CommandInfo info;
    info.command = NULL;
    info.arg_count = 0;
    info.redirect_in = NULL;
    info.redirect_out = NULL;
    info.redirect_append = NULL;

    char *p = input_str;

    while (*p != '\0' && info.arg_count < MAX_ARGS - 1)
    {
        // Skip whitespace
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
            break;

        // Mark start of token
        char *token_start = p;
        char *write = p;

        while (*p != '\0')
        {
            if (*p == '\\' && *(p + 1) != '\0' && !isspace((unsigned char)*(p + 1)))
            {
                p++;             // skip backslash
                *write++ = *p++; // copy next char literally
            }
            else if (*p == '"' || *p == '\'')
            {
                char quote = *p++;
                while (*p != '\0')
                {
                    if (quote == '"' && *p == '\\' && *(p + 1) != '\0')
                    {
                        p++;
                        *write++ = *p++;
                    }
                    else if (*p == quote)
                        break;
                    else
                        *write++ = *p++;
                }
                if (*p == quote)
                    p++;
            }
            else if (isspace((unsigned char)*p))
                break;
            else
                *write++ = *p++;
        }
        char delimiter = *p;
        *write = '\0'; // null-terminate token

        if (write > token_start || p > token_start)
        {
            if (info.arg_count == 0)
                info.command = token_start;
            info.args[info.arg_count++] = token_start;
        }

        if (isspace((unsigned char)delimiter))
            p++; // step past space that ended token
    }

    info.args[info.arg_count] = NULL;
    int new_count = 0;
    for (int i = 0; i < info.arg_count; i++)
    {
        if (strcmp(info.args[i], ">>") == 0 && i + 1 < info.arg_count)
            info.redirect_append = info.args[++i];
        else if (strcmp(info.args[i], ">") == 0 && i + 1 < info.arg_count)
            info.redirect_out = info.args[++i];
        else if (strcmp(info.args[i], "<") == 0 && i + 1 < info.arg_count)
            info.redirect_in = info.args[++i];
        else
            info.args[new_count++] = info.args[i];
    }
    info.arg_count = new_count;
    info.args[new_count] = NULL;
    if (new_count > 0)
        info.command = info.args[0];
    return info;
}

bool arg_matches(const char *argvi, const char *shortarg, const char *longarg)
{
    if (shortarg != NULL && (strcmp(argvi, shortarg) == 0))
    {
        return true;
    }
    if (longarg != NULL && (strcmp(argvi, longarg) == 0))
    {
        return true;
    }
    return false;
}

void about_command()
{
    printf("sebsh %s\n", VER);
}

void help_command()
{
    printf("sebsh - builtin commands:\n");
    printf("  help           show this help message\n");
    printf("  ver            show version\n");
    printf("  exit/quit      exit the shell\n");
    printf("  cd <dir>       change directory\n");
    printf("  debug          toggle debug messages\n");
#ifdef _WIN32 // Because on windows, these aren't executables
    printf("  ls [dir] [-r]  list directory contents\n");
    printf("  dir [dir] [-r] functions the same as ls\n");
    printf("  echo [message] echoes a message\n");
    printf("  clear          clear the screen\n");
#endif
}

#ifdef _WIN32
void list_directory(const char *path, bool recursive)
{
    if (path == NULL || *path == '\0')
        path = ".";

    char search_path[FILENAME_MAX];
    size_t len = strlen(path);
    if (len > 0 && (path[len - 1] == '\\' || path[len - 1] == '/'))
        snprintf(search_path, sizeof(search_path), "%s*", path);
    else
        snprintf(search_path, sizeof(search_path), "%s\\*", path);

    struct _finddata_t fileinfo;
    intptr_t handle = _findfirst(search_path, &fileinfo);
    if (handle == -1)
    {
        perror(path);
        return;
    }

    do
    {
        if (strcmp(fileinfo.name, ".") == 0 || strcmp(fileinfo.name, "..") == 0)
            continue;

        printf("%s\n", fileinfo.name);

        if (recursive && (fileinfo.attrib & _A_SUBDIR))
        {
            char subdir[FILENAME_MAX];
            snprintf(subdir, sizeof(subdir), "%s\\%s", path, fileinfo.name);
            list_directory(subdir, true);
        }
    } while (_findnext(handle, &fileinfo) == 0);

    _findclose(handle);
}
#endif

void process_command(CommandInfo *info, sebsh *state)
{

    if (info->command == NULL)
        return;

    if (state->debug)
    {
        printf("Command is %s \n", info->command);
        for (int i = 0; i < info->arg_count; i++)
        {
            printf("Arg %d: %s\n", i, info->args[i]);
        }
    }

    if (strcmp(info->command, "help") == 0)
    {
        help_command();
    }
    else if (strcmp(info->command, "ver") == 0)
    {
        about_command();
    }
    else if (strcmp(info->command, "exit") == 0 || strcmp(info->command, "quit") == 0)
    {
        printf("exit\n");
        state->running = false;
    }
    else if (strcmp(info->command, "cd") == 0)
    {
        const char *dir = info->args[1];

        if (dir == NULL)
        {
            dir = getenv("HOME");
            if (dir == NULL)
            {
                fprintf(stderr, "cd: HOME not set\n");
                return;
            }
        }
#ifdef _WIN32
        if (_chdir(dir) != 0)
        {
            perror("cd");
        }
#else
        if (chdir(dir) != 0)
        {
            perror("cd");
        }
#endif
    }
    else if (strcmp(info->command, "debug") == 0)
    {
        state->debug = !state->debug;
        printf("Debug messages are now %s.\n", state->debug ? "enabled" : "disabled");
    }
#ifdef _WIN32
    else if (strcmp(info->command, "ls") == 0 || strcmp(info->command, "dir") == 0)
    {
        bool recursive = false;
        const char *dir = NULL;
        for (int i = 1; i < info->arg_count; i++)
        {
            if (strcmp(info->args[i], "-r") == 0)
                recursive = true;
            else
                dir = info->args[i];
        }
        list_directory(dir, recursive);
    }
    else if (strcmp(info->command, "echo") == 0)
    {
        for (int i = 1; i < info->arg_count; i++)
        {
            if (i > 1)
                putchar(' ');
            fputs(info->args[i], stdout);
        }
        putchar('\n');
    }
    else if (strcmp(info->command, "clear") == 0)
    {
        system("cls");
    }
#endif
    else
    {
        spawn_process(info->command, info->args, info->redirect_in, info->redirect_out, info->redirect_append);
    }
}

char *join_args(int argc, char **argv, int i)
{
    size_t len = 0;
    // Calculate required memory (including spaces)
    for (int j = i; j < argc; j++)
        len += strlen(argv[j]) + 1;

    // Allocate buffer and concatenate
    char *res = calloc(len, 1);
    if (res == NULL)
    {
        perror("calloc");
        return NULL;
    }
    for (int j = i; j < argc; j++)
    {
        strcat(res, argv[j]);
        if (j < argc - 1)
            strcat(res, " ");
    }
    return res;
}

int main(int argc, char *argv[])
{
    char command[CMD_SIZE] = {0};
    CommandInfo result;
    sebsh state = {
        .running = true,
        .debug = false,
        .cwd = {0},
        .prompt = " #> "};

    signal(SIGINT, handle_interrupt);
#ifndef _WIN32
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    acquire_controlling_tty();
    suppress_echoctl();
#endif

    for (int i = 1; i < argc; i++)
    {
        if (arg_matches(argv[i], "-v", "--version"))
        {
            about_command();
            return 0;
        }
        else if (arg_matches(argv[i], "-c", "--command"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "sebsh: -c requires a command\n");
                return 1;
            }
            char *joined = join_args(argc, argv, i + 1); // build command string
            if (joined == NULL)
            {
                state.running = false;
                break;
            }
            i = argc;

            char *segments[MAX_PIPE_SEGMENTS];
            int seg_count = split_pipe(trim(joined), segments, MAX_PIPE_SEGMENTS);

            if (seg_count == 1)
            {
                result = parse_input(segments[0]);
                if (result.command != NULL)
                    process_command(&result, &state);
            }
            else
            {
#ifdef _WIN32
                fprintf(stderr, "sebsh: piping not supported on Windows\n");
#else
                CommandInfo pipeline[MAX_PIPE_SEGMENTS];
                for (int j = 0; j < seg_count; j++)
                    pipeline[j] = parse_input(trim(segments[j]));
                execute_pipeline(pipeline, seg_count);
#endif
            }

            free(joined);
            state.running = false;
        }
        else
        {
            fprintf(stderr, "sebsh: Too many or unknown argument(s)!\n");
        }
    }

    while (state.running)
    {
        interrupt_requested = 0;
        if (GETCWD(state.cwd, sizeof(state.cwd)) == NULL)
        {
            perror("GETCWD");
            return 1;
        }
        printf("%s%s", state.cwd, state.prompt);
        fflush(stdout);

        if (fgets(command, sizeof(command), stdin) == NULL)
        {
            if (interrupt_requested && !feof(stdin))
            {
                clearerr(stdin);
                interrupt_requested = 0;
                continue;
            }
            printf("\n");
            break;
        }

        interrupt_requested = 0;

        char *segments[MAX_PIPE_SEGMENTS];
        int seg_count = split_pipe(trim(command), segments, MAX_PIPE_SEGMENTS);

        if (seg_count == 1)
        {
            result = parse_input(segments[0]);
            if (result.command != NULL)
                process_command(&result, &state);
        }
        else
        {
#ifdef _WIN32
            fprintf(stderr, "sebsh: piping not supported on Windows\n");
#else
            CommandInfo pipeline[MAX_PIPE_SEGMENTS];
            for (int j = 0; j < seg_count; j++)
                pipeline[j] = parse_input(trim(segments[j]));
            execute_pipeline(pipeline, seg_count);
#endif
        }
        fflush(stdout);
    }

    return 0;
}
