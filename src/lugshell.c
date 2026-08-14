/*
Basic Grammar:
==============

list        ::= conjunction
              | conjunction LIST-SYMB list

conjunction ::= disjunction
              | disjunction AND-SYMB conjunction

disjunction ::= pipeline
              | pipeline OR-SYMB disjunction

pipeline    ::= command
              | command PIPE-SYMB pipeline

LIST-SYMB ::= ';'
AND-SYMB  ::= '&&'
OR-SYMB   ::= '||'
PIPE-SYMB ::= '|'

 */

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/wait.h>

#include <readline/readline.h>
#include <readline/history.h>

static jmp_buf sigint_buf;

static void handle_signal(int) {
  siglongjmp(sigint_buf, 1);
}

static int do_echo() {
  fprintf(stdout, "echo!!!\n");
  return 0;
}

static struct builtin_action {
  char* name;
  int (*action)();
} builtins[] = {
  { "echo", do_echo }
};

static void chomp(char* str) {
  int len = strlen(str);
  if (len == 0) {
    return;
  }
  if (str[len - 1] == '\n') {
    str[len - 1] = '\0';
  }
}

static struct builtin_action* find_builtin(char* cmd) {
  int elem_count = sizeof(builtins) / sizeof(builtins[0]);
  for (int i = 0; i != elem_count; ++i) {
    if (strcmp(builtins[i].name, cmd) == 0) {
      return &builtins[i];
    }
  }
  return NULL;
}

static int invoke_cmd(int in, int out, char* args[]) {
  pid_t pid = fork();
  if (pid < 0) {
    // forking error
  } else if (pid == 0) {
    // the child
    if (in != STDIN_FILENO) {
      dup2(in, STDIN_FILENO);
      close(in);
    }
    if (out != STDOUT_FILENO) {
      dup2(out, STDOUT_FILENO);
      close(out);
    }
    execvp(args[0], args);
    fprintf(stdout, "Exec failed: %s\n", strerror(errno));
    exit(1);
  } else {
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
    return -1; // error?
  }
}

static int process_pipeline(int in, int out, char* cmd[]) {
  char** cmd_start = &cmd[0];
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

static int process_disjunction(int in, int out, char* cmd[]) {
  char** pipeline_start = &cmd[0];
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

static int process_conjunction(int in, int out, char* cmd[]) {
  char** disj_start = &cmd[0];
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

static int process_list(int in, int out, char* cmd[]) {
  char** conj_start = &cmd[0];
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

static void process(int in, int out, char* input, int count) {
  if (count == 0) {
    return;
  }

  // Tokenize the input line into an array of strings, one per
  // token, separated by whitespace. Final string in the array is a
  // NULL pointer.
  char* tokens[1024] = {};
  char* saveptr = NULL;
  {
    int i = 0;
    for (char* to_tokenize = input; i != 1023; to_tokenize = NULL, ++i) {
      char* token = strtok_r(to_tokenize, " ", &saveptr);
      if (token == NULL) {
        break;
      }
      tokens[i] = token;
    }
    tokens[i] = NULL;
  }

  if (struct builtin_action* action = find_builtin(tokens[0])) {
    // handle any carved-out builtin functions.
    (void) (action->action)();
  } else {
    // Fall back to regular invocation.
    process_list(in, out, tokens);
  }
}

int main() {
  // Do startup things.

  signal(SIGINT, handle_signal);

  char* path = getenv("PATH");

  fprintf(stdout, "lugshell! Heck yeah!\n");
  fprintf(stdout, path);
  fputc('\n', stdout);
  fflush(stdout);

  if (sigsetjmp(sigint_buf, 1) == 1) {
    // When we catch a Ctrl-C, we do a longjump from the signal
    // handler and wind up back here. Put out a newline so the
    // output to the user looks cleaner, then immediately fall back
    // into the read/eval loop.
    fputc('\n', stdout);
  }

  char* input;
  while ((input = readline("% ")) != NULL) {
    if (input[0] != '\0') {
      add_history(input); // Add non-empty lines to history
      chomp(input);
      process(STDIN_FILENO, STDOUT_FILENO, input, strlen(input));
    }
    free(input); // Readline allocates memory that must be freed
  }
}
