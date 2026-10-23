/**
 * @file lugshell.c
 * @brief Implements a simple shell in C.
 *
 * Basic Grammar
 * =============
 *
 * list        ::= conjunction
 *               | conjunction LIST-SYMB list
 *
 * conjunction ::= disjunction
 *               | disjunction AND-SYMB conjunction
 *
 * disjunction ::= pipeline
 *               | pipeline OR-SYMB disjunction
 *
 * pipeline    ::= command
 *               | command PIPE-SYMB pipeline
 *
 * LIST-SYMB ::= ';'
 * AND-SYMB  ::= '&&'
 * OR-SYMB   ::= '||'
 * PIPE-SYMB ::= '|'
 *
 * @author Bryan St. Amour, Randy Topliffe
 */

#include <errno.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef USE_READLINE
#include <readline/history.h>
#include <readline/readline.h>
#endif

static jmp_buf sigint_buf;

struct builtin_action;

static void handle_signal(int);
static void chomp(char *str);
static struct builtin_action *find_builtin(char *cmd);
static int invoke_action(struct builtin_action *, char **);
static int invoke_cmd(int in, int out, char *args[]);
static int process_pipeline(int in, int out, char *cmd[]);
static int process_disjunction(int in, int out, char *cmd[]);
static int process_conjunction(int in, int out, char *cmd[]);
static int process_list(int in, int out, char *cmd[]);
static void process(int in, int out, char *input, int count);

/**
 * @brief main entrypoint for the program.
 *
 * @details Sets up the signal handlers and other initial state,
 * then loops, reading user input via Readline, and evaluates their
 * command string and prints the result.
 */
int main() {
    // Do startup things.

    signal(SIGINT, handle_signal);

    char *path = getenv("PATH");

    fprintf(stdout, "lugshell! Heck yeah!\n");
    fprintf(stdout, "%s", path);
    fputc('\n', stdout);
    fflush(stdout);

    if (sigsetjmp(sigint_buf, 1) == 1) {
        // When we catch a Ctrl-C, we do a longjump from the signal
        // handler and wind up back here. Put out a newline so the
        // output to the user looks cleaner, then immediately fall back
        // into the read/eval loop.
        // See setjmp.3 manual for more information.
        //
        // The reason for sigsetjmp and siglongjmp are because when you
        // catch a signal, your currently caught signal is masked and thus
        // cannot be caught a second time. The sig* versions not only perform
        // the jump, but also unmask the signal.
        fputc('\n', stdout);
    }

#ifdef USE_READLINE
    char *input;
    while ((input = readline("% ")) != NULL) {
        if (input[0] != '\0') {
            add_history(input);
            chomp(input);
            process(STDIN_FILENO, STDOUT_FILENO, input, strlen(input));
        }
        free(input); // Readline allocates memory that must be freed
    }
#else
    while (1) {
        char buffer[1024];
        fprintf(stdout, "%% ");
        fflush(stdout);
        fgets(buffer, sizeof(buffer) - 1, stdin);
        if (feof(stdin)) {
            break;
        }
        chomp(buffer);
        process(STDIN_FILENO, STDOUT_FILENO, buffer, strlen(buffer));
    }
#endif
}

/**
 * @brief Signal handler. Jumps back to main via longjump.
 */
static void handle_signal(int) {
    siglongjmp(sigint_buf, 1);
}

/**
 * @brief If the last character of the provided string is a newline,
 * then remove it (by converting it to a null character).
 *
 * @param str The string to chomp.
 */
static void chomp(char *str) {
    int len = strlen(str);
    if (len == 0) {
        return;
    }
    if (str[len - 1] == '\n') {
        str[len - 1] = '\0';
    }
}

/**
 * @brief Tokenizes the input string, and then evaluates the
 * token stream.
 *
 * @param in The file descriptor for standard in.
 * @param out The file descriptor for standard out.
 * @param input The input string read from the user.
 * @param count The length of the input string.
 */
static void process(int in, int out, char *input, int count) {
    if (count == 0) {
        return;
    }

    // Tokenize the input line into an array of strings, one per
    // token, separated by whitespace. Final string in the array is a
    // NULL pointer.
    const int max_tokens = 1024;
    char *tokens[1024] = {};
    char *saveptr = NULL;
    {
        int i = 0;
        for (char *to_tokenize = input; i != max_tokens - 1;
             to_tokenize = NULL, ++i) {
            // strtok_r works by the following means:
            //
            // The first time you call it, you pass it a string to tokenize
            // (our to_tokenize pointer), a delimiter string, and some state
            // (saveptr). Subsequent calls are done by passing NULL as your
            // first parameter. Each call you make returns a pointer to the
            // start of the next token in the string. We save those pointers
            // in our tokens array for downstream processing.
            char *token = strtok_r(to_tokenize, " ", &saveptr);
            if (token == NULL) {
                break;
            }
            tokens[i] = token;
        }
        tokens[i] = NULL;
    }

    struct builtin_action *action = find_builtin(tokens[0]);
    if (action) {
        // handle any carved-out builtin functions.
        invoke_action(action, &tokens[1]);
    } else {
        // Fall back to regular invocation.
        process_list(in, out, tokens);
    }
}

/**
 * @brief Implements built-in echo functionality.
 */
static int do_echo(char **tokens) {
    for (int i = 0; tokens[i] != NULL; ++i) {
        fprintf(stdout, tokens[i]);
        if (tokens[i + 1] != NULL) {
            fprintf(stdout, " ");
        }
    }
    fprintf(stdout, "\n");
    return 0;
}

/**
 * @brief Implements cd (change directory) functionality.
 */
static int do_cd(char **tokens) {
    char *path_to_cd = tokens[0];
    if (path_to_cd == NULL) {
        // No path, equivalent to "cd" by itself. Go to the user's home
        // directory.
        struct passwd *pw = getpwuid(getuid());
        if (pw != NULL) {
            int result = chdir(pw->pw_dir);
            if (result == -1) {
                fprintf(stdout, "Cannot cd: %s\n", strerror(errno));
            }
        } else {
            fprintf(stdout, "Cannot find user home directory\n");
        }
    } else {
        switch (path_to_cd[0]) {
        // These first few choices are not yet implemented.
        case '-':
        case '~':
            break;
        default:
            if (chdir(path_to_cd) == -1) {
                fprintf(stdout, "Cannot cd: %s\n", strerror(errno));
            }
            break;
        }
    }
    return 0;
}

//
// A table of built-in shell commands. Whenever you want to add a new 
// built-in, write the functionality, then add it to this table. It will
// automatically work with the rest of the shell.
//
static struct builtin_action {
    char *name;
    int (*action)(char **);
} builtins[] = {{"echo", do_echo}, {"cd", do_cd}};

/**
 * @brief Given an action entry and a command line parameter list,
 * invoke the action on the parameters.
 */
static int invoke_action(struct builtin_action *action, char **tokens) {
    return action->action(tokens);
}

/**
 * @brief Finds the associated builtin action.
 *
 * @param cmd The name of the action to find.
 * @returns A pointer to the associated builtin_action struct,
 * or a null pointer if no such action is found.
 */
static struct builtin_action *find_builtin(char *cmd) {
    const int elem_count = sizeof(builtins) / sizeof(builtins[0]);
    for (int i = 0; i != elem_count; ++i) {
        if (strcmp(builtins[i].name, cmd) == 0) {
            return &builtins[i];
        }
    }
    return NULL;
}

/**
 * @brief Processes the list cmd ; cmd ; ... ; cmd
 *
 * @param in The file descriptor for standard in.
 * @param out The file descriptor for standard out.
 * @param args The argument list. The final char* must be null.
 * @returns The exit status of the final conjunction in the list.
 */
static int process_list(int in, int out, char *cmd[]) {
    char **conj_start = &cmd[0];
    int result = 0;
    int i = 0;
    while (1) {
        if (cmd[i] == NULL) {
            result = process_conjunction(in, out, conj_start);
            break;
        } else if (strcmp(cmd[i], ";") == 0) {
            cmd[i] = NULL;
            result = process_conjunction(in, out, conj_start);
            conj_start = &cmd[i + 1];
        }
        ++i;
    }
    return result;
}

/**
 * @brief Processes the conjunction cmd && cmd && ... && cmd
 *
 * @param in The file descriptor for standard in.
 * @param out The file descriptor for standard out.
 * @param args The argument list. The final char* must be null.
 * @returns The exit status of the conjunction of exist statuses.
 *
 * @details This function evaluates its disjunctions left-to-right,
 * and short-circuits: it stops evaluating disjunctions once the
 * first disjunction that returns a non-zero exit status is run.
 */
static int process_conjunction(int in, int out, char *cmd[]) {
    char **disj_start = &cmd[0];
    int result = 0;
    int i = 0;
    while (1) {
        if (cmd[i] == NULL) {
            result = process_disjunction(in, out, disj_start);
            break;
        } else if (strcmp(cmd[i], "&&") == 0) {
            cmd[i] = NULL;
            result = process_disjunction(in, out, disj_start);
            if (result != 0) {
                break;
            }
            disj_start = &cmd[i + 1];
        }
        ++i;
    }
    return result;
}

/**
 * @brief Processes the disjunction cmd || cmd || ... || cmd
 *
 * @param in The file descriptor for standard in.
 * @param out The file descriptor for standard out.
 * @param args The argument list. The final char* must be null.
 * @returns The exit status of the disjunction of exist statuses.
 *
 * @details This function evaluates its pipelines left-to-right, and
 * short-circuits: it stops evaluating pipelines once the first
 * pipeline that returns a zero exit status is run.
 */
static int process_disjunction(int in, int out, char *cmd[]) {
    char **pipeline_start = &cmd[0];
    int result = 0;
    int i = 0;
    while (1) {
        if (cmd[i] == NULL) {
            result = process_pipeline(in, out, pipeline_start);
            break;
        } else if (strcmp(cmd[i], "||") == 0) {
            cmd[i] = NULL;
            result = process_pipeline(in, out, pipeline_start);
            if (result == 0) {
                break;
            }
            pipeline_start = &cmd[i + 1];
        }
        ++i;
    }
    return result;
}

/**
 * @brief Processes the pipeline cmd | cmd | ... | cmd
 *
 * @param in The file descriptor for standard in.
 * @param out The file descriptor for standard out.
 * @param args The argument list. The final char* must be null.
 * @returns The exit status of the final command of the pipeline.
 */
static int process_pipeline(int in, int out, char *cmd[]) {
    char **cmd_start = &cmd[0];
    int i = 0;
    while (1) {
        if (cmd[i] == NULL) {
            return invoke_cmd(in, out, cmd_start);
        } else if (strcmp(cmd[i], "|") == 0) {
            // Kick off a new pipeline.
            cmd[i] = NULL;
            int pipe_fd[2];
            pipe(pipe_fd);
            (void)invoke_cmd(in, pipe_fd[1], cmd_start);
            in = pipe_fd[0];
            cmd_start = &cmd[i + 1]; // Skip the nulled pipe.
        }
        ++i;
    }
}

/**
 * @brief Invokes the simple command "cmd arg1 arg2 ...".
 *
 * @param in The file descriptor for standard in.
 * @param out The file descriptor for standard out.
 * @param args The argument list. The final char* must be null.
 * @returns The exit status of the invoked command.
 */
static int invoke_cmd(int in, int out, char *args[]) {
    pid_t pid = fork();
    if (pid < 0) {
        // Forking error. Report and exit. We probably don't want to
        // stick around if we cannot create a new process.
        fprintf(stdout, "Fork failed: %s\n", strerror(errno));
        exit(1);
    } else if (pid == 0) {
        // We are in the child process here.
        //
        // If we were given a pipe to read/write from, then duplicate
        // those descriptors over stdin and stdout, and close the old
        // ones. Then, replace the current process image with that of
        // the program we wish to run. That program will run, inheriting
        // our file descriptors.
        if (in != STDIN_FILENO) {
            dup2(in, STDIN_FILENO);
            close(in);
        }
        if (out != STDOUT_FILENO) {
            dup2(out, STDOUT_FILENO);
            close(out);
        }
        execvp(args[0], args);

        // If we reach this point, we failed to exec() a new process
        // image. So report such, and then exit the child process.
        fprintf(stdout, "Exec failed: %s\n", strerror(errno));
        exit(1);
    } else {
        // We are in the parent process here.
        //
        // If the provided in/out descriptors are different than
        // standard in and out, close them. Then, wait for the child
        // process to complete, and collect its exit status.
        if (in != STDIN_FILENO) {
            close(in);
        }
        if (out != STDOUT_FILENO) {
            close(out);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return -1; // error reading the status.
    }
}
