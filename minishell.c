// minishell.c
// Build: gcc -Wall -Wextra -O2 minishell.c -o minishell
// Run:   ./minishell

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
  char **argv;        // null-terminated argv
  int argc;
  char *in_file;      // for <
  char *out_file;     // for > or >>
  bool out_append;    // true for >>
} Command;

typedef struct {
  Command *cmds;      // pipeline commands
  int ncmds;
  bool background;    // trailing &
} Pipeline;

/* --------------------- Utility --------------------- */

static void die(const char *msg) {
  perror(msg);
  exit(1);
}

static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p) {
    fprintf(stderr, "Out of memory\n");
    exit(1);
  }
  return p;
}

static char *xstrdup(const char *s) {
  char *d = strdup(s);
  if (!d) {
    fprintf(stderr, "Out of memory\n");
    exit(1);
  }
  return d;
}

/* --------------------- Tokenizer --------------------- */
/*
  Splits input into tokens supporting:
  - whitespace separation
  - quotes: '...' and "..."
  - operators: |, <, >, >>, &
*/

typedef struct {
  char **toks;
  int ntoks;
} Tokens;

static bool is_op_char(char c) {
  return (c == '|' || c == '<' || c == '>' || c == '&');
}

static Tokens tokenize(const char *line) {
  Tokens T = {0};
  int cap = 16;
  T.toks = (char **)xmalloc(sizeof(char *) * cap);
  T.ntoks = 0;

  size_t i = 0, n = strlen(line);
  while (i < n) {
    while (i < n && isspace((unsigned char)line[i])) i++;
    if (i >= n) break;

    // Operators
    if (is_op_char(line[i])) {
      char op[3] = {0, 0, 0};
      op[0] = line[i];

      if (line[i] == '>' && i + 1 < n && line[i + 1] == '>') {
        op[1] = '>';
        i += 2;
      } else {
        i += 1;
      }

      if (T.ntoks == cap) {
        cap *= 2;
        T.toks = (char **)realloc(T.toks, sizeof(char *) * cap);
        if (!T.toks) die("realloc");
      }
      T.toks[T.ntoks++] = xstrdup(op);
      continue;
    }

    // Word / quoted string
    char quote = 0;
    if (line[i] == '"' || line[i] == '\'') {
      quote = line[i++];
    }

    size_t start = i;
    char *buf = NULL;
    size_t blen = 0, bcap = 64;
    buf = (char *)xmalloc(bcap);

    while (i < n) {
      char c = line[i];

      if (quote) {
        if (c == quote) { i++; break; }
        // allow escaping inside double quotes for \" and \\ and \n \t
        if (quote == '"' && c == '\\' && i + 1 < n) {
          char next = line[i + 1];
          char out = next;
          if (next == 'n') out = '\n';
          else if (next == 't') out = '\t';
          // else keep next as-is (\", \\ etc)
          if (blen + 1 >= bcap) { bcap *= 2; buf = (char *)realloc(buf, bcap); if (!buf) die("realloc"); }
          buf[blen++] = out;
          i += 2;
          continue;
        }
        if (blen + 1 >= bcap) { bcap *= 2; buf = (char *)realloc(buf, bcap); if (!buf) die("realloc"); }
        buf[blen++] = c;
        i++;
      } else {
        if (isspace((unsigned char)c) || is_op_char(c)) break;
        if (c == '"' || c == '\'') break; // stop before quote if mixed
        if (blen + 1 >= bcap) { bcap *= 2; buf = (char *)realloc(buf, bcap); if (!buf) die("realloc"); }
        buf[blen++] = c;
        i++;
      }
    }

    // If unquoted and next char begins quote, handle as separate tokens (simple behavior).
    // Finalize token
    buf[blen] = '\0';

    // If we started with quote, token is in buf.
    // If we didn't start with quote and didn't consume anything (rare), skip.
    if (quote || blen > 0) {
      if (T.ntoks == cap) {
        cap *= 2;
        T.toks = (char **)realloc(T.toks, sizeof(char *) * cap);
        if (!T.toks) die("realloc");
      }
      T.toks[T.ntoks++] = buf;
    } else {
      free(buf);
    }

    // If we stopped because we hit a quote while unquoted, loop will handle it next.
    (void)start;
  }

  return T;
}

static void free_tokens(Tokens *T) {
  for (int i = 0; i < T->ntoks; i++) free(T->toks[i]);
  free(T->toks);
  T->toks = NULL;
  T->ntoks = 0;
}

/* --------------------- Parser --------------------- */

static void command_init(Command *c) {
  c->argv = NULL;
  c->argc = 0;
  c->in_file = NULL;
  c->out_file = NULL;
  c->out_append = false;
}

static void command_add_arg(Command *c, const char *arg) {
  int cap = (c->argv ? c->argc + 2 : 8);
  if (!c->argv) {
    c->argv = (char **)xmalloc(sizeof(char *) * cap);
  } else {
    // grow sometimes
    // ensure space for +1 arg + NULL
    c->argv = (char **)realloc(c->argv, sizeof(char *) * cap);
    if (!c->argv) die("realloc");
  }

  c->argv[c->argc++] = xstrdup(arg);
  c->argv[c->argc] = NULL;
}

static void command_free(Command *c) {
  if (c->argv) {
    for (int i = 0; i < c->argc; i++) free(c->argv[i]);
    free(c->argv);
  }
  free(c->in_file);
  free(c->out_file);
  command_init(c);
}

static void pipeline_free(Pipeline *p) {
  for (int i = 0; i < p->ncmds; i++) command_free(&p->cmds[i]);
  free(p->cmds);
  p->cmds = NULL;
  p->ncmds = 0;
  p->background = false;
}

static bool parse_tokens(const Tokens *T, Pipeline *out) {
  out->cmds = NULL;
  out->ncmds = 0;
  out->background = false;

  if (T->ntoks == 0) return false;

  int cap_cmds = 4;
  out->cmds = (Command *)xmalloc(sizeof(Command) * cap_cmds);
  out->ncmds = 1;
  command_init(&out->cmds[0]);

  int ci = 0;
  for (int i = 0; i < T->ntoks; i++) {
    const char *tok = T->toks[i];

    if (strcmp(tok, "|") == 0) {
      // start next command
      if (out->cmds[ci].argc == 0) {
        fprintf(stderr, "syntax error: empty command before pipe\n");
        pipeline_free(out);
        return false;
      }
      if (out->ncmds == cap_cmds) {
        cap_cmds *= 2;
        out->cmds = (Command *)realloc(out->cmds, sizeof(Command) * cap_cmds);
        if (!out->cmds) die("realloc");
      }
      ci++;
      out->ncmds++;
      command_init(&out->cmds[ci]);
      continue;
    }

    if (strcmp(tok, "&") == 0) {
      // only valid at end
      if (i != T->ntoks - 1) {
        fprintf(stderr, "syntax error: '&' must be at end\n");
        pipeline_free(out);
        return false;
      }
      out->background = true;
      continue;
    }

    if (strcmp(tok, "<") == 0) {
      if (i + 1 >= T->ntoks) {
        fprintf(stderr, "syntax error: missing input file after '<'\n");
        pipeline_free(out);
        return false;
      }
      free(out->cmds[ci].in_file);
      out->cmds[ci].in_file = xstrdup(T->toks[++i]);
      continue;
    }

    if (strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
      bool append = (tok[1] == '>');
      if (i + 1 >= T->ntoks) {
        fprintf(stderr, "syntax error: missing output file after '>'\n");
        pipeline_free(out);
        return false;
      }
      free(out->cmds[ci].out_file);
      out->cmds[ci].out_file = xstrdup(T->toks[++i]);
      out->cmds[ci].out_append = append;
      continue;
    }

    // regular arg
    command_add_arg(&out->cmds[ci], tok);
  }

  // final validation
  for (int k = 0; k < out->ncmds; k++) {
    if (out->cmds[k].argc == 0) {
      fprintf(stderr, "syntax error: empty command in pipeline\n");
      pipeline_free(out);
      return false;
    }
  }

  return true;
}

/* --------------------- Execution --------------------- */

static int open_infile(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) fprintf(stderr, "minishell: cannot open input '%s': %s\n", path, strerror(errno));
  return fd;
}

static int open_outfile(const char *path, bool append) {
  int flags = O_WRONLY | O_CREAT;
  flags |= append ? O_APPEND : O_TRUNC;
  int fd = open(path, flags, 0644);
  if (fd < 0) fprintf(stderr, "minishell: cannot open output '%s': %s\n", path, strerror(errno));
  return fd;
}

static bool is_builtin(const Command *c) {
  if (c->argc == 0) return false;
  return (strcmp(c->argv[0], "cd") == 0 || strcmp(c->argv[0], "exit") == 0);
}

static int run_builtin(const Command *c) {
  if (strcmp(c->argv[0], "exit") == 0) {
    int code = 0;
    if (c->argc >= 2) code = atoi(c->argv[1]);
    exit(code);
  }

  if (strcmp(c->argv[0], "cd") == 0) {
    const char *target = NULL;
    if (c->argc >= 2) {
      target = c->argv[1];
    } else {
      target = getenv("HOME");
      if (!target) target = ".";
    }

    if (chdir(target) != 0) {
      fprintf(stderr, "minishell: cd: %s: %s\n", target, strerror(errno));
      return 1;
    }
    return 0;
  }

  return 0;
}

static void reap_background(void) {
  int status;
  pid_t pid;
  // Reap all completed children without blocking
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    if (WIFEXITED(status)) {
      fprintf(stderr, "[bg] pid %d exited (%d)\n", pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      fprintf(stderr, "[bg] pid %d killed by signal %d\n", pid, WTERMSIG(status));
    }
  }
}

static int exec_pipeline(const Pipeline *p) {
  // Builtins only supported when it's a single command and foreground
  if (p->ncmds == 1 && !p->background && is_builtin(&p->cmds[0])) {
    return run_builtin(&p->cmds[0]);
  }

  int n = p->ncmds;
  int prev_read = -1;
  pid_t *pids = (pid_t *)xmalloc(sizeof(pid_t) * n);

  for (int i = 0; i < n; i++) {
    int pipefd[2] = {-1, -1};
    bool is_last = (i == n - 1);

    if (!is_last) {
      if (pipe(pipefd) != 0) die("pipe");
    }

    pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
      // Child
      // Restore default SIGINT in foreground jobs
      signal(SIGINT, SIG_DFL);

      // If we have a previous pipe read end, hook it to stdin
      if (prev_read != -1) {
        if (dup2(prev_read, STDIN_FILENO) < 0) die("dup2 prev_read");
      }

      // If not last, hook stdout to pipe write end
      if (!is_last) {
        close(pipefd[0]); // close read end in child
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) die("dup2 pipe write");
      }

      // Redirections for this command
      const Command *c = &p->cmds[i];

      if (c->in_file) {
        int fd = open_infile(c->in_file);
        if (fd < 0) _exit(1);
        if (dup2(fd, STDIN_FILENO) < 0) die("dup2 infile");
        close(fd);
      }
      if (c->out_file) {
        int fd = open_outfile(c->out_file, c->out_append);
        if (fd < 0) _exit(1);
        if (dup2(fd, STDOUT_FILENO) < 0) die("dup2 outfile");
        close(fd);
      }

      // Close inherited fds
      if (prev_read != -1) close(prev_read);
      if (!is_last) close(pipefd[1]);

      // If command is a builtin in a pipeline/background, run it in child
      if (is_builtin(c)) {
        int rc = run_builtin(c);
        _exit(rc);
      }

      execvp(c->argv[0], c->argv);
      fprintf(stderr, "minishell: %s: %s\n", c->argv[0], strerror(errno));
      _exit(127);
    }

    // Parent
    pids[i] = pid;

    if (prev_read != -1) close(prev_read);
    if (!is_last) {
      close(pipefd[1]);       // close write end in parent
      prev_read = pipefd[0];  // keep read end for next command
    } else {
      prev_read = -1;
    }
  }

  int status = 0;
  int last_status = 0;

  if (p->background) {
    fprintf(stderr, "[bg] started pid %d\n", pids[n - 1]);
    free(pids);
    return 0;
  }

  // Foreground: wait all; return last command's status like many shells
  for (int i = 0; i < n; i++) {
    if (waitpid(pids[i], &status, 0) < 0) {
      perror("waitpid");
      continue;
    }
    if (i == n - 1) {
      if (WIFEXITED(status)) last_status = WEXITSTATUS(status);
      else if (WIFSIGNALED(status)) last_status = 128 + WTERMSIG(status);
    }
  }

  free(pids);
  return last_status;
}

/* --------------------- Main Loop --------------------- */

static void print_prompt(void) {
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd))) {
    printf("minishell:%s$ ", cwd);
  } else {
    printf("minishell$ ");
  }
  fflush(stdout);
}

int main(void) {
  // Ignore SIGINT in the shell itself; children restore default.
  signal(SIGINT, SIG_IGN);

  char *line = NULL;
  size_t cap = 0;

  while (1) {
    reap_background();
    print_prompt();

    ssize_t r = getline(&line, &cap, stdin);
    if (r < 0) {
      // EOF (Ctrl-D)
      printf("\n");
      break;
    }

    // strip trailing newline
    if (r > 0 && line[r - 1] == '\n') line[r - 1] = '\0';

    // skip empty
    char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') continue;

    Tokens T = tokenize(line);
    Pipeline pipex = {0};

    if (parse_tokens(&T, &pipex)) {
      (void)exec_pipeline(&pipex);
      pipeline_free(&pipex);
    }

    free_tokens(&T);
  }

  free(line);
  return 0;
}