#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Educational limits: reject excess input instead of silently truncating it. */
#define MAX_TOKENS 512
#define MAX_CMDS 32
#define MAX_ARGS 128
#define MAX_REDIRS 32
#define MAX_JOBS 64

typedef enum { WORD, PIPE, INPUT, OUTPUT, APPEND, BACKGROUND } Kind;
typedef struct { Kind kind; char *text; } Token;
typedef struct { Kind kind; char *path; } Redirection;
typedef struct {
    char *argv[MAX_ARGS + 1];
    int argc, nr;
    Redirection redir[MAX_REDIRS];
} Command;
typedef struct {
    Command cmd[MAX_CMDS];
    int count;
    bool background, explain, profile;
} Pipeline;
typedef struct { pid_t pid; int state, status; } Process; /* 0 running, 1 stopped, 2 done */
typedef struct {
    bool used, background, profile, has_modes;
    int id, count;
    pid_t pgid;
    Process process[MAX_CMDS];
    char *line;
    struct timespec start;
    struct termios modes;
} Job;

static Job jobs[MAX_JOBS];
static int next_id = 1, last_status;
static bool interactive, exiting, theme;
static pid_t shell_pgid;
static int terminal_fd = -1;
static struct termios shell_modes;
static volatile sig_atomic_t interrupted;

static void die(const char *what) { perror(what); exit(EXIT_FAILURE); }
static void *allocate(size_t n) { void *p = calloc(1, n); if (!p) die("calloc"); return p; }
static void set_signal(int sig, void (*handler)(int)) {
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL) < 0) die("sigaction");
}
static void on_interrupt(int sig) { (void)sig; interrupted = 1; }
static void colors(bool on) {
    theme = on;
    if (interactive) {
        fputs(on ? "\033[48;2;35;25;55m\033[38;2;235;225;245m" : "\033[0m", stdout);
        fflush(stdout);
    }
}

/* Lexer: preserve quoted/escaped operators as ordinary word characters. */
static int lex(const char *s, Token *t) {
    int n = 0;
    while (*s) {
        if (isspace((unsigned char)*s)) { s++; continue; }
        if (n == MAX_TOKENS) goto error;
        if (strchr("|<>&", *s)) {
            Kind k = *s == '|' ? PIPE : *s == '<' ? INPUT : *s == '&' ? BACKGROUND : OUTPUT;
            s++;
            if (k == OUTPUT && *s == '>') { k = APPEND; s++; }
            t[n++] = (Token){k, NULL};
            continue;
        }
        char *word = allocate(strlen(s) + 1);
        size_t length = 0;
        char quote = 0;
        while (*s) {
            char ch = *s;
            if (!quote && (isspace((unsigned char)ch) || strchr("|<>&", ch))) break;
            if (!quote && (ch == '\'' || ch == '"')) { quote = ch; s++; continue; }
            if (quote && ch == quote) { quote = 0; s++; continue; }
            if (ch == '\\' && quote != '\'') {
                if (!s[1]) { free(word); goto error; }
                /* In double quotes, retain backslashes before ordinary letters. */
                if (!quote || strchr("\\\"$`", s[1])) { s++; ch = *s; }
            }
            word[length++] = ch;
            s++;
        }
        if (quote) { free(word); goto error; }
        word[length] = '\0';
        t[n++] = (Token){WORD, word};
    }
    return n;
error:
    for (int i = 0; i < n; i++) free(t[i].text);
    fprintf(stderr, "Syntax error: unmatched quote, trailing escape, or too many tokens\n");
    return -1;
}

static bool parse(Token *t, int n, Pipeline *p) {
    int i = 0;
    if (!n) return true;
    if (t[0].kind == WORD && (!strcmp(t[0].text, "explain") || !strcmp(t[0].text, "profile"))) {
        p->explain = !strcmp(t[0].text, "explain");
        p->profile = !strcmp(t[0].text, "profile");
        i++;
    }
    if (i == n) goto error;
    p->count = 1;
    for (; i < n; i++) {
        Command *c = &p->cmd[p->count - 1];
        if (t[i].kind == WORD) {
            if (c->argc == MAX_ARGS) goto error;
            c->argv[c->argc++] = t[i].text;
        } else if (t[i].kind == PIPE) {
            if (!c->argc || p->count == MAX_CMDS) goto error;
            p->count++;
        } else if (t[i].kind == BACKGROUND) {
            if (i != n - 1 || !c->argc) goto error;
            p->background = true;
        } else {
            Kind k = t[i].kind;
            if (++i == n || t[i].kind != WORD || c->nr == MAX_REDIRS) goto error;
            c->redir[c->nr++] = (Redirection){k, t[i].text};
        }
    }
    if (!p->cmd[p->count - 1].argc) goto error;
    return true;
error:
    fprintf(stderr, "Syntax error: expected command/path or exceeded parser limits\n");
    return false;
}

/* Reap children outside signal handlers; no allocation or printf in handlers. */
static void record_status(pid_t pid, int status) {
    for (int j = 0; j < MAX_JOBS; j++) if (jobs[j].used)
        for (int i = 0; i < jobs[j].count; i++) {
            Process *p = &jobs[j].process[i];
            if (p->pid != pid) continue;
            p->status = status;
            p->state = WIFSTOPPED(status) ? 1 : WIFCONTINUED(status) ? 0 : 2;
            return;
        }
}
static bool done(Job *j) {
    for (int i = 0; i < j->count; i++) if (j->process[i].state != 2) return false;
    return true;
}
static bool stopped(Job *j) {
    bool any = false;
    for (int i = 0; i < j->count; i++) {
        if (!j->process[i].state) return false;
        if (j->process[i].state == 1) any = true;
    }
    return any;
}
static int exit_code(int s) {
    if (WIFEXITED(s)) return WEXITSTATUS(s);
    if (WIFSIGNALED(s)) return 128 + WTERMSIG(s);
    if (WIFSTOPPED(s)) return 128 + WSTOPSIG(s);
    return 0;
}
static double elapsed(struct timespec start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
}
static void release_job(Job *j) {
    if (j->profile) {
        fprintf(stderr, "[profile] elapsed %.6f s (includes any stopped time)\n", elapsed(j->start));
        for (int i = 0; i < j->count; i++)
            fprintf(stderr, "[profile] stage %d pid %ld status %d\n", i + 1,
                    (long)j->process[i].pid, exit_code(j->process[i].status));
    }
    free(j->line);
    memset(j, 0, sizeof(*j));
}
static void reap(void) {
    int s;
    pid_t pid;
    while ((pid = waitpid(-1, &s, WNOHANG | WUNTRACED | WCONTINUED)) > 0) record_status(pid, s);
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && done(&jobs[i])) {
        if (interactive) fprintf(stderr, "[%d] Done %s\n", jobs[i].id, jobs[i].line);
        release_job(&jobs[i]);
    }
}
static void mark_running(Job *j) {
    for (int i = 0; i < j->count; i++) if (j->process[i].state == 1) j->process[i].state = 0;
}
static int foreground(Job *j, bool resume) {
    j->background = false;
    if (interactive) {
        if (tcsetpgrp(terminal_fd, j->pgid) < 0) perror("tcsetpgrp");
        if (resume && j->has_modes) tcsetattr(terminal_fd, TCSADRAIN, &j->modes);
    }
    if (resume) {
        if (kill(-j->pgid, SIGCONT) < 0) perror("SIGCONT");
        mark_running(j);
    }
    while (!done(j) && !stopped(j)) {
        int s;
        pid_t pid = waitpid(-j->pgid, &s, WUNTRACED);
        if (pid > 0) record_status(pid, s);
        else if (pid < 0 && errno != EINTR) { perror("waitpid"); break; }
    }
    if (interactive) {
        if (stopped(j)) j->has_modes = tcgetattr(terminal_fd, &j->modes) == 0;
        tcsetpgrp(terminal_fd, shell_pgid);
        tcsetattr(terminal_fd, TCSADRAIN, &shell_modes);
    }
    int result = exit_code(j->process[j->count - 1].status);
    if (done(j)) release_job(j);
    else {
        j->background = true;
        fprintf(stderr, "[%d] Stopped %s\n", j->id, j->line);
    }
    return result;
}

static int builtin_id(const char *s) {
    const char *names[] = {"createFile", "createFolder", "openFolder", "cd", "openFile",
                          "pwd", "help", "exit", "jobs", "fg", "bg", "theme", "status", NULL};
    for (int i = 0; names[i]; i++) if (!strcmp(s, names[i])) return i + 1;
    return 0;
}
static bool needs_parent(int id) { return id == 8 || id == 9 || id == 10 || id == 11 || id == 12; }
static int builtin(Command *c, bool child) {
    int id = builtin_id(c->argv[0]);
    char **a = c->argv;
    if (child && needs_parent(id)) {
        fprintf(stderr, "%s: use as a standalone foreground command\n", a[0]); return 2;
    }
    if (id == 1) {
        bool overwrite = c->argc == 3 && !strcmp(a[1], "--overwrite");
        if (c->argc != 2 && !overwrite) goto usage;
        char *name = a[overwrite ? 2 : 1];
        int fd = open(name, O_WRONLY | O_CREAT | (overwrite ? O_TRUNC : O_EXCL), 0666);
        if (fd < 0) { perror(name); return 1; }
        if (close(fd) < 0) { perror("close"); return 1; }
        printf("Created: %s\n", name);
    } else if (id == 2) {
        if (c->argc != 2) goto usage;
        if (mkdir(a[1], 0755) < 0) { perror(a[1]); return 1; }
    } else if (id == 3 || id == 4) {
        if (c->argc > 2) goto usage;
        const char *path = c->argc == 2 ? a[1] : getenv("HOME");
        if (!path) { fprintf(stderr, "HOME is not set\n"); return 1; }
        if (chdir(path) < 0) { perror(path); return 1; }
    } else if (id == 5) {
        if (c->argc != 2) goto usage;
        FILE *f = fopen(a[1], "r");
        if (!f) { perror(a[1]); return 1; }
        char buffer[4096]; size_t n; int result = 0;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
            if (fwrite(buffer, 1, n, stdout) != n) { result = 1; break; }
        if (ferror(f)) { fprintf(stderr, "%s: read failed\n", a[1]); result = 1; }
        if (fclose(f) != 0) result = 1;
        return result;
    } else if (id == 6) {
        if (c->argc != 1) goto usage;
        char *cwd = getcwd(NULL, 0);
        if (!cwd) { perror("getcwd"); return 1; }
        puts(cwd); free(cwd);
    } else if (id == 7) {
        puts("createFile PATH [use: createFile --overwrite PATH to replace]\n"
             "createFolder PATH | openFolder [PATH] | cd [PATH] | openFile PATH\n"
             "pwd | status | jobs | fg ID | bg ID | theme on/off | help | exit\n"
             "External programs: ls -l, gcc --version, python3 ...\n"
             "Syntax: quotes, escapes, |, <, >, >>, trailing &\n"
             "explain COMMAND: inspect without executing\n"
             "profile COMMAND: execute and report elapsed time and stage statuses\n"
             "Not implemented: variables, globs, &&, ||, ;, scripts, 2>, substitution.");
    } else if (id == 8) {
        if (c->argc != 1) goto usage;
        exiting = true;
    } else if (id == 9) {
        if (c->argc != 1) goto usage;
        reap();
        for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used)
            printf("[%d] %-7s pgid=%ld %s\n", jobs[i].id,
                   stopped(&jobs[i]) ? "Stopped" : "Running", (long)jobs[i].pgid, jobs[i].line);
    } else if (id == 10 || id == 11) {
        if (c->argc != 2) goto usage;
        reap();
        char *end; long jid = strtol(a[1], &end, 10);
        if (!*a[1] || *end || jid <= 0) goto usage;
        Job *j = NULL;
        for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used && jobs[i].id == jid) j = &jobs[i];
        if (!j) { fprintf(stderr, "No such active job: %s\n", a[1]); return 1; }
        if (id == 10) return foreground(j, true);
        if (kill(-j->pgid, SIGCONT) < 0) { perror("SIGCONT"); return 1; }
        mark_running(j); j->background = true;
        printf("[%d] Running %s\n", j->id, j->line);
    } else if (id == 12) {
        if (c->argc != 2 || (strcmp(a[1], "on") && strcmp(a[1], "off"))) goto usage;
        colors(!strcmp(a[1], "on"));
    } else if (id == 13) {
        if (c->argc != 1) goto usage;
        printf("%d\n", last_status);
    }
    return 0;
usage:
    fprintf(stderr, "%s: invalid arguments; type help for usage\n", a[0]);
    return 2;
}

static int redirect(Command *c) {
    /* Apply redirects left to right, after pipeline connections. */
    for (int i = 0; i < c->nr; i++) {
        Redirection *r = &c->redir[i];
        int target = r->kind == INPUT ? STDIN_FILENO : STDOUT_FILENO;
        int flags = r->kind == INPUT ? O_RDONLY : O_WRONLY | O_CREAT |
                    (r->kind == APPEND ? O_APPEND : O_TRUNC);
        int fd = open(r->path, flags, 0666);
        if (fd < 0) { perror(r->path); return -1; }
        if (dup2(fd, target) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    }
    return 0;
}
static void explain(Pipeline *p) {
    printf("Mode: %s; stages: %d\n", p->background ? "background" : "foreground", p->count);
    for (int i = 0; i < p->count; i++) {
        Command *c = &p->cmd[i];
        printf("Stage %d (%s)\n", i + 1, builtin_id(c->argv[0]) ? "built-in" : "external");
        for (int a = 0; a < c->argc; a++) printf("  argv[%d] = [%s]\n", a, c->argv[a]);
        printf("  default input: %s; default output: %s\n",
               i ? "previous pipe" : "shell stdin", i + 1 < p->count ? "next pipe" : "shell stdout");
        for (int r = 0; r < c->nr; r++)
            printf("  then redirect %s [%s]\n", c->redir[r].kind == INPUT ? "<" :
                   c->redir[r].kind == APPEND ? ">>" : ">", c->redir[r].path);
    }
}
static int run_parent(Command *c, bool profile) {
    fflush(NULL);
    int saved_in = dup(STDIN_FILENO), saved_out = dup(STDOUT_FILENO);
    if (saved_in < 0 || saved_out < 0) die("dup");
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    int result = redirect(c) < 0 ? 1 : builtin(c, false);
    if (fflush(stdout) == EOF) result = 1;
    if (dup2(saved_in, STDIN_FILENO) < 0 || dup2(saved_out, STDOUT_FILENO) < 0) die("restore descriptors");
    close(saved_in); close(saved_out); clearerr(stdin); clearerr(stdout);
    if (profile) fprintf(stderr, "[profile] built-in pid %ld status %d elapsed %.6f s\n",
                         (long)getpid(), result, elapsed(start));
    return result;
}

static int execute(Pipeline *p, const char *line) {
    if (p->explain) { explain(p); return 0; }
    if (!p->background && p->count == 1 && builtin_id(p->cmd[0].argv[0]))
        return run_parent(&p->cmd[0], p->profile);
    for (int i = 0; i < p->count; i++) if (needs_parent(builtin_id(p->cmd[i].argv[0]))) {
        fprintf(stderr, "%s: use as a standalone foreground command\n", p->cmd[i].argv[0]); return 2;
    }
    reap();
    Job *j = NULL;
    for (int i = 0; i < MAX_JOBS; i++) if (!jobs[i].used) { j = &jobs[i]; break; }
    if (!j) { fprintf(stderr, "Job table full\n"); return 1; }
    j->used = true; j->id = next_id++; j->background = p->background; j->profile = p->profile;
    j->line = strdup(line); if (!j->line) die("strdup");
    clock_gettime(CLOCK_MONOTONIC, &j->start);
    int pipes[MAX_CMDS - 1][2], created = 0, gate[2] = {-1, -1};
    for (int i = 0; i < p->count - 1; i++) {
        if (pipe(pipes[i]) < 0) { perror("pipe"); goto fail; }
        created++;
    }
    /* A launch gate prevents foreground children reading before terminal handoff. */
    if (pipe(gate) < 0) { perror("pipe"); goto fail; }
    fflush(NULL);
    for (int i = 0; i < p->count; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); goto fail; }
        if (!pid) {
            if (setpgid(0, j->pgid ? j->pgid : getpid()) < 0) _exit(1);
            int signals[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGCHLD};
            for (size_t s = 0; s < sizeof(signals)/sizeof(signals[0]); s++) set_signal(signals[s], SIG_DFL);
            close(gate[1]);
            char byte;
            while (read(gate[0], &byte, 1) < 0 && errno == EINTR) {}
            close(gate[0]);
            if (i && dup2(pipes[i - 1][0], STDIN_FILENO) < 0) _exit(1);
            if (i + 1 < p->count && dup2(pipes[i][1], STDOUT_FILENO) < 0) _exit(1);
            if (!interactive && p->background && !i) {
                int fd = open("/dev/null", O_RDONLY);
                if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) _exit(1);
                close(fd);
            }
            for (int k = 0; k < created; k++) { close(pipes[k][0]); close(pipes[k][1]); }
            if (redirect(&p->cmd[i]) < 0) _exit(1);
            if (builtin_id(p->cmd[i].argv[0])) {
                int result = builtin(&p->cmd[i], true);
                if (fflush(NULL) == EOF) result = 1;
                _exit(result);
            }
            execvp(p->cmd[i].argv[0], p->cmd[i].argv);
            int err = errno;
            perror(p->cmd[i].argv[0]);
            _exit(err == ENOENT ? 127 : 126);
        }
        if (!j->pgid) j->pgid = pid;
        j->process[j->count++] = (Process){pid, 0, 0};
        if (setpgid(pid, j->pgid) < 0 && errno != EACCES) { perror("setpgid"); goto fail; }
    }
    for (int k = 0; k < created; k++) { close(pipes[k][0]); close(pipes[k][1]); }
    if (interactive && !p->background && tcsetpgrp(terminal_fd, j->pgid) < 0) perror("tcsetpgrp");
    close(gate[0]); close(gate[1]);
    if (p->background) { printf("[%d] %ld\n", j->id, (long)j->pgid); return 0; }
    return foreground(j, false);
fail:
    for (int k = 0; k < created; k++) { close(pipes[k][0]); close(pipes[k][1]); }
    /* Kill children while the gate is closed so a partial pipeline cannot execute. */
    for (int i = 0; i < j->count; i++) kill(j->process[i].pid, SIGKILL);
    if (gate[0] >= 0) close(gate[0]);
    if (gate[1] >= 0) close(gate[1]);
    for (int i = 0; i < j->count; i++) while (waitpid(j->process[i].pid, NULL, 0) < 0 && errno == EINTR) {}
    j->profile = false; release_job(j);
    return 1;
}

int main(void) {
    interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (interactive) {
        terminal_fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
        if (terminal_fd < 0) die("terminal descriptor");
        set_signal(SIGTTIN, SIG_DFL);
        while (tcgetpgrp(terminal_fd) != getpgrp()) kill(-getpgrp(), SIGTTIN);
        set_signal(SIGTTOU, SIG_IGN); set_signal(SIGTSTP, SIG_IGN);
        shell_pgid = getpid();
        if (setpgid(0, shell_pgid) < 0 && getpgrp() != shell_pgid) die("setpgid");
        if (tcsetpgrp(terminal_fd, shell_pgid) < 0) die("tcsetpgrp");
        if (tcgetattr(terminal_fd, &shell_modes) < 0) die("tcgetattr");
        set_signal(SIGINT, on_interrupt); set_signal(SIGQUIT, SIG_IGN);
        colors(true);
    }
    set_signal(SIGCHLD, SIG_DFL);
    set_signal(SIGPIPE, SIG_IGN);
    /* Avoid stdio reading ahead into input intended for child programs. */
    setvbuf(stdin, NULL, _IONBF, 0);
    char *line = NULL; size_t capacity = 0;
    while (!exiting) {
        reap(); interrupted = 0;
        if (interactive) { printf("qcsh> "); fflush(stdout); }
        errno = 0;
        if (getline(&line, &capacity, stdin) < 0) {
            if (errno == EINTR) { clearerr(stdin); if (interactive) puts(""); last_status = 130; continue; }
            break;
        }
        if (interrupted) { last_status = 130; continue; }
        line[strcspn(line, "\n")] = '\0';
        Token tokens[MAX_TOKENS] = {0};
        int n = lex(line, tokens);
        if (n < 0) { last_status = 2; continue; }
        Pipeline *p = allocate(sizeof(*p));
        if (!parse(tokens, n, p)) last_status = 2;
        else if (p->count) last_status = execute(p, line);
        free(p);
        for (int i = 0; i < n; i++) free(tokens[i].text);
    }
    free(line);
    /* Deliberate policy: active jobs receive SIGHUP and SIGCONT on shell exit. */
    for (int i = 0; i < MAX_JOBS; i++) if (jobs[i].used) {
        kill(-jobs[i].pgid, SIGHUP); kill(-jobs[i].pgid, SIGCONT);
        free(jobs[i].line);
    }
    if (interactive) { colors(false); tcsetattr(terminal_fd, TCSADRAIN, &shell_modes); }
    if (terminal_fd >= 0) close(terminal_fd);
    return last_status;
}
