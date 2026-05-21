// sebsh, by Sebminecrafter
// sebsh © 2026 by Sebminecrafter is licensed under CC BY-SA 4.0. To view a copy of this license, visit https://creativecommons.org/licenses/by-sa/4.0/

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

bool debug = false;

// Cross-platform process spawn function
int spawn_process(const char *path, char *const argv[])
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
    else if (pid == 0)
    {
        execvp(path, argv);
        perror("execvp");
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

bool arg_matches(char argvi[], char single[], char shortarg[], char longarg[])
{
    if (single != NULL && (strcmp(argvi, single) == 0))
    {
        return true;
    }
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
    printf("sebsh - builtin commands:\n", VER);
    printf("  help        show this help message\n");
    printf("  ver         show version\n");
    printf("  exit/quit   exit the shell\n");
    printf("  cd <dir>    change directory\n");
    printf("  debug       toggle debug messages\n");
}

void process_command(char *command, char *args[], bool *running)
{
    if (command == NULL)
        return;

    if (debug)
    {
        printf("Command is %s \n", command);
        for (int i = 0; i < (sizeof(args[0]) / sizeof(args)); i++)
        {
            printf("Arg %d: %s\n", i, args[i]);
        }
    }

    if (strcmp(command, "help") == 0)
    {
        help_command();
    }
    else if (strcmp(command, "ver") == 0)
    {
        about_command();
    }
    else if (strcmp(command, "exit") == 0 || strcmp(command, "quit") == 0)
    {
        printf("exit\n");
        *running = false;
    }
    else if (strcmp(command, "cd") == 0)
    {
        const char *dir = args[1];
        if (dir == NULL)
        {
            perror("cd");
        }
#ifdef _WIN32
        else if (_chdir(dir) != 0)
        {
            perror("cd");
        }
#else
        else if (chdir(dir) != 0)
        {
            perror("cd");
        }
#endif
    }
    else if (strcmp(command, "debug") == 0)
    {
        debug = !debug;
        printf("Debug messages are now %s.\n", debug ? "enabled" : "disabled");
    }
    else
    {
        spawn_process(command, args);
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
    bool running = true;
    char cwd[FILENAME_MAX];
    char command[CMD_SIZE];
    CommandInfo result;

    for (int i = 1; i < argc; i++)
    {
        if (arg_matches(argv[i], "version", "-v", "--version"))
        {
            about_command();
            return 0;
        }
        else if (arg_matches(argv[i], NULL, "-c", "--command"))
        {
            result = parse_input(trim(join_args(argc, argv, i++)));
            process_command(result.command, result.args, &running);
            running = false;
        }
        else
        {
            fprintf(stderr, "sebsh: Too many or unknown argument(s)!\n");
        }
    }

    while (running)
    {
        if (GETCWD(cwd, sizeof(cwd)) == NULL)
        {
            perror("GETCWD");
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
