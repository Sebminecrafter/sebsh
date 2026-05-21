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
#define CMD_SIZE 4096
#define MAX_ARGS 100

typedef struct
{
    bool debug;
    bool running;
    char *cwd[FILENAME_MAX];
} sebsh;

typedef struct
{
    char *command;
    char *args[MAX_ARGS];
    int arg_count;
} CommandInfo;

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

CommandInfo parse_input(char *input_str)
{
    CommandInfo info;
    info.command = NULL;
    info.arg_count = 0;

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
            if (*p == '"' || *p == '\'')
            {
                // Enter quoted section, skip opening quote
                char quote = *p++;
                while (*p != '\0' && *p != quote)
                    *write++ = *p++;
                if (*p == quote)
                    p++; // skip closing quote
            }
            else if (isspace((unsigned char)*p))
            {
                break; // end of token
            }
            else
            {
                *write++ = *p++;
            }
        }
        *write = '\0'; // null-terminate token

        if (write > token_start || p > token_start)
        {
            if (info.arg_count == 0)
                info.command = token_start;
            info.args[info.arg_count++] = token_start;
        }

        if (isspace((unsigned char)*p))
            p++; // step past space that ended token
    }

    info.args[info.arg_count] = NULL;
    return info;
}

bool arg_matches(char argvi[], char shortarg[], char longarg[])
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
    printf("  help        show this help message\n");
    printf("  ver         show version\n");
    printf("  exit/quit   exit the shell\n");
    printf("  cd <dir>    change directory\n");
    printf("  debug       toggle debug messages\n");
}

void process_command(char *command, char *args[], int arg_count, sebsh state)
{
    if (command == NULL)
        return;

    if (state.debug)
    {
        printf("Command is %s \n", command);
        for (int i = 0; i < arg_count; i++)
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
        state.running = false;
    }
    else if (strcmp(command, "cd") == 0)
    {
        const char *dir = args[1];

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
    else if (strcmp(command, "debug") == 0)
    {
        state.debug = !state.debug;
        printf("Debug messages are now %s.\n", state.debug ? "enabled" : "disabled");
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
    sebsh state = {
        .debug = false,
        .running = true,
        .cwd = {0}};

    state.running = true;
    state.debug = false;
    char command[CMD_SIZE];
    CommandInfo result;

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
            char *joined = join_args(argc, argv, i + 1);
            if (joined == NULL)
            {
                free(joined);
                state.running = false;
                break;
            }
            result = parse_input(trim(joined));
            i = argc;
            process_command(result.command, result.args, result.arg_count, state);
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
        if (GETCWD(state.cwd, sizeof(state.cwd)) == NULL)
        {
            perror("GETCWD");
            return 1;
        }
        printf("%s> ", state.cwd);
        fflush(stdout);

        if (fgets(command, sizeof(command), stdin) == NULL)
        {
            printf("\n");
            break;
        }

        result = parse_input(trim(command));
        if (result.command != NULL)
        {
            process_command(result.command, result.args, result.arg_count, state);
        }
        fflush(stdout);
    }

    return 0;
}
