#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/wait.h>

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

static void invoke_cmd(char* args[]) {
  pid_t pid = fork();
  if (pid < 0) {
    // forking error
  } else if (pid == 0) {
    // the child
    execvp(args[0], args);

    fprintf(stdout, "Exec failed: %s\n", strerror(errno));
    exit(1);
  } else {
    // the parent
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
      // it exited.
    }
  }
}

static void process(char* input, int count) {
  if (count == 0) {
    return;
  }

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
    invoke_cmd(tokens);
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
    fputc('\n', stdout);
  }
  while (1) {
    char buffer[1024];
    fprintf(stdout, "%% ");
    fflush(stdout);
    fgets(buffer, sizeof(buffer) - 1, stdin);
    if (feof(stdin)) {
      break;
    }
    chomp(buffer);
    // Process the line we read.
    process(buffer, strlen(buffer));
  }
}
