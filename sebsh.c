#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define GETCWD _getcwd
#else
#include <unistd.h>
#include <sys/wait.h>
#define GETCWD getcwd
#endif

#define VER "1.0.0"
#define CMD_SIZE 1000
#define MAX_ARGS 100

void err_at(const char *module)
{
    fprintf(stderr, "Error at ");
    perror(module);
}

// Cross-platform process spawn function
int spawn_process(const char *path, char *const argv[])
{
#ifdef _WIN32
    int result = _spawnvp(_P_WAIT, path, argv);
    if (result == -1)
    {
        err_at("_spawnvp");
    }
    return result;
#else
    pid_t pid = fork();
    if (pid < 0)
    {
        err_at("fork");
        return -1;
    }
    else if (pid == 0)
    {
        execvp(path, argv);
        err_at("execvp");
        exit(EXIT_FAILURE);
    }
    else
    {
        int status;
        waitpid(pid, &status, 0);
        return WEXITSTATUS(status);
    }
#endif
}

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

typedef struct
{
    char *command;
    char *args[MAX_ARGS];
    int arg_count;
} CommandInfo;

CommandInfo parse_input(char *input_str)
{
    CommandInfo info;
    info.command = NULL;
    info.arg_count = 0;

    char *token = strtok(input_str, " ");
    if (token != NULL)
    {
        info.command = trim(token);
        info.args[info.arg_count++] = info.command;

        while (info.arg_count < MAX_ARGS - 1)
        {
            token = strtok(NULL, " ");
            if (token == NULL)
                break;
            info.args[info.arg_count++] = token;
        }
        info.args[info.arg_count] = NULL;
    }
    return info;
}

void process_command(char *command, char *args[], bool *running)
{
    if (command == NULL)
        return;

    if (strcmp(command, "help") == 0)
    {
        printf("sebsh %s - available commands:\n", VER);
        printf("  help        show this help message\n");
        printf("  ver         show version\n");
        printf("  exit/quit   exit the shell\n");
        printf("  cd <dir>    change directory\n");
        printf("  <command>   run an external command\n");
    }
    else if (strcmp(command, "ver") == 0)
    {
        printf("sebsh %s\n", VER);
    }
    else if (strcmp(command, "exit") == 0 || strcmp(command, "quit") == 0)
    {
        *running = false;
    }
    else if (strcmp(command, "cd") == 0)
    {
        const char *dir = args[1];
        if (dir == NULL)
        {
            fprintf(stderr, "cd: missing argument\n");
        }
#ifdef _WIN32
        else if (_chdir(dir) != 0)
        {
            err_at("cd");
        }
#else
        else if (chdir(dir) != 0)
        {
            err_at("cd");
        }
#endif
    }
    else
    {
        spawn_process(command, args);
    }
}

int main(int argc, char *argv[])
{
    bool running = true;
    char cwd[FILENAME_MAX];
    char command[CMD_SIZE];
    CommandInfo result;

    if (argc >= 2 && (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0))
    {
        printf("sebsh %s\n", VER);
        return 0;
    }

    while (running)
    {
        if (GETCWD(cwd, sizeof(cwd)) == NULL)
        {
            err_at("GETCWD");
            return 1;
        }
        printf("%s> ", cwd);
        fflush(stdout);

        if (fgets(command, sizeof(command), stdin) == NULL)
        {
            printf("\n");
            break;
        }

        result = parse_input(trim(command));
        if (result.command != NULL)
        {
            process_command(result.command, result.args, &running);
        }
        fflush(stdout);
    }

    return 0;
}
