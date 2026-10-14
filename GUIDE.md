# qcsh: setup, commands, and implementation guide

## 1. What changed in your supplied code

After renaming `banao` to `createFile`, you left the old length `6` in
`strncmp` and `input + 6`. The new prefix `createFile ` is 11 characters long.
That means your original code would match too little and start the filename
inside the command name. The same issue affected other renamed commands:

| Prefix including its following space | Correct length in the old design |
|---|---:|
| `createFile ` | 11 |
| `openFolder ` | 11 |
| `createFolder ` | 13 |
| `openFile ` | 9 |

The new implementation avoids this fragile approach. It tokenizes a line and
compares the complete first argument with command names. For example,
`createFile "my notes.txt"` becomes two arguments: `createFile`, `my notes.txt`.
The command then reads argument 1; no manually counted offset is involved.

Other changes: dynamically sized input, syntax checking before execution,
functions instead of a large main loop, file-preserving creation by default,
and process/pipe/job machinery. This is a replacement main.c, not a block to
paste into the old while loop.

## 2. Install the files and run

Extract the downloaded ZIP using Windows File Explorer. Its qcsh directory
contains main.c, this guide, README.md, Makefile, tests, .gitignore, and a GitHub
workflow. Open your existing ~/projects/my-shell folder in VS Code through WSL.
Back up your old main.c before copying the new main.c into that folder. Copy
other supplied files alongside it; preserve your existing .git directory.
Merge rather than overwrite unrelated README or workflow content if needed.

In the Ubuntu/WSL terminal, while outside your running qcsh:

```bash
cd ~/projects/my-shell
cp main.c main.before-upgrade.c
```

Do the copy above BEFORE replacing main.c. Then replace it in VS Code and save.
Compile and run:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -g main.c -o qcsh
./qcsh
```

- `gcc`: compile, assemble, and link your source.
- `-std=c11`: request the C11 language standard.
- `-Wall -Wextra -Wpedantic`: request useful warning diagnostics.
- `-g`: retain debugger information.
- `main.c`: source input.
- `-o qcsh`: executable output name.
- `./qcsh`: launch that executable from the current directory.

If build tools are missing, install them in Ubuntu using
`sudo apt update` followed by `sudo apt install build-essential python3`.

With all archive files copied, `make` builds, `make test` checks behavior,
`make sanitize` builds a sanitizer version and tests it, and `make clean`
removes only the two generated executables. Test commands are run from Bash.

## 3. Custom file commands

### createFile PATH

```text
createFile notes.txt
createFile "my notes.txt"
```

Creates a new, empty file in your current directory unless you specify another
path. If it already exists, creation fails and content is preserved. This uses
`open` with `O_CREAT | O_EXCL | O_WRONLY`, followed by `close`. The OS performs
creation and the existence check as one exclusive operation.

```text
createFile --overwrite notes.txt
```

This explicit variant uses `O_TRUNC`: it empties an existing file. Do not use it
on content you want to keep. Requested permissions are 0666, filtered by umask.

### createFolder PATH

```text
createFolder practice
createFolder "OS Practice"
```

Calls `mkdir` with mode 0755 (also filtered by umask). Creates one directory;
its parent must already exist. An existing path causes an error.

### openFolder PATH / cd PATH

```text
openFolder practice
cd ..
openFolder
```

`chdir` changes the current process's working directory. `..` is its parent.
No argument uses the HOME environment variable. This does not open a GUI window.
For a standalone built-in, it executes in the parent shell, so subsequent
commands use the new directory. Inside a pipeline/background job it changes
only a child's directory and is generally not useful.

### openFile PATH

```text
openFile notes.txt
```

Opens a file for reading with `fopen`, reads chunks with `fread`, and writes them
with `fwrite`. It preserves file bytes without adding an extra newline, which
makes it suitable for a pipeline. It prints content rather than launching an
editor. Reading binary files can display meaningless/control characters.

### pwd

Prints the current directory using `getcwd`. Useful after `openFolder`.

Paths containing spaces now need quotes or escaped spaces. The earlier parser
accepted the entire rest of the line as a filename; this one separates arguments.
Use full paths such as `/mnt/c/Users/khush/Desktop` when working with Windows
files through WSL. Verify your real Desktop location (possibly OneDrive).

## 4. External programs

```text
ls -l
whoami
gcc --version
printf "hello\n"
```

The shell finds a command among its built-ins first. Otherwise it starts a child
with `fork` and calls `execvp` in that child. `execvp` uses PATH for names without
a slash. It replaces the child program with the requested executable; it does
not return on success. The parent retains the prompt/input loop and waits for
the foreground job.

`ls`, `whoami`, `gcc`, and `printf` are existing external programs here; qcsh
contains no implementations of their functionality. Error status 127 denotes a
missing command; 126 denotes another execution failure in this implementation.

## 5. Quotes and escapes

```text
createFolder "my folder"
createFile 'literal name.txt'
printf "%s\n" "hello | world"
```

Quoted spaces stay within a single argument. A quoted pipe is text, not a pipe
operator. Both quote types remove their delimiters from the resulting argument.
Empty quoted arguments are retained. Backslash escapes outside single quotes
are supported. Inside double quotes, backslashes before ordinary characters
are retained; e.g. external printf receives `\n` and interprets it itself.
Unmatched quotes and trailing escapes produce a syntax error.

Variable expansion is not implemented: `$HOME` stays literal. Use `openFolder`
without an argument or provide the actual path. This is a deliberately small
syntax, not Bash compatibility.

## 6. Pipes and redirection

```text
printf "OS\nDBMS\nOS\n" | grep OS | wc -l
```

Should print 2. Three processes run concurrently. `pipe` creates channels;
`dup2` replaces each stage's standard input/output with the appropriate ends.
All unused descriptors are closed in the parent and children, so EOF propagates.
The parent launches every stage before waiting. Waiting too early can deadlock
when a producer fills a pipe before its consumer starts.

```text
printf "OS notes\n" > notes.txt
printf "More notes\n" >> notes.txt
wc -l < notes.txt
openFile notes.txt | grep OS
```

- `>` creates/truncates an output file. Unlike createFile, this intentionally
  follows conventional overwrite semantics.
- `>>` creates or appends to an output file.
- `<` supplies a file as standard input.
- Redirections override pipeline defaults and are applied left to right.
- Example: `printf hi > a > b` empties/creates a and writes hi to b.
- Redirection belongs to the stage where it appears, not every stage.
- `2>` and `2>&1` are not supported.

A standalone built-in temporarily redirects the shell's descriptors, executes,
flushes output, and restores the original descriptors. Otherwise `pwd > file`
could accidentally send later prompts into the same file.

## 7. Background jobs and signals

```text
sleep 30 &
jobs
fg 1
```

`&` must appear at the end. It returns a prompt while the job runs. The displayed
bracketed number is the job ID; use YOUR displayed ID with fg/bg. IDs can have
gaps because foreground jobs also allocate IDs internally.

`jobs` reports Running/Stopped jobs and their process group IDs. A pipeline is
one job containing several processes in a shared process group.

- Ctrl+C: terminal sends SIGINT to the foreground process group. Child programs
  normally terminate. The shell regains the terminal and displays its prompt.
- Ctrl+Z: terminal sends SIGTSTP to the foreground group. Stopped processes stay
  in memory; the shell records them as a stopped job.
- `bg ID`: send SIGCONT to resume a job without handing it the terminal.
- `fg ID`: transfer the terminal to a job, send SIGCONT, and wait for it.
- Ctrl+D at an empty prompt: end-of-input; exit the shell.

Try `sleep 30`, Ctrl+Z, `jobs`, `bg ID`, then `fg ID` and Ctrl+C. Background
programs that attempt to read the terminal may stop with SIGTTIN; use fg to let
them read. The implementation uses setpgid and tcsetpgrp, not merely waitpid.

There is a launch gate: children wait on a small pipe until all children are in
their group and the parent has assigned the terminal. This prevents a new
foreground reader from mistakenly being stopped as a background reader.

Job reaping occurs before prompts and during job commands. There is no immediate
notification while waiting for input. Press Enter to trigger an update. This is
an explicit limitation; a later self-pipe/event-loop implementation can improve it.

## 8. explain and profile

```text
explain openFile notes.txt | grep OS > matching.txt
```

Prints each stage, built-in/external classification, arguments, default I/O,
and ordered redirections. It uses the real parsed structure. It does NOT run
commands, open paths, or create matching.txt. A missing input file will not be
reported here because explain deliberately does not try to open it.

```text
profile openFile notes.txt | grep OS | wc -l
profile sleep 1
```

Executes the pipeline, then reports elapsed wall-clock seconds and each stage's
PID and numeric status to stderr. A standalone built-in runs in the parent, so
its report identifies the shell PID. A background profile is printed on reaping.
Timing includes stopped time and, for background jobs, possible reaping delay.
No CPU percentage, CPU time, or memory measurements are claimed.

Explain/profile are special first words applying to the whole pipeline. Do not
combine them (e.g. `explain profile ...` is not nested modifier syntax).

## 9. Remaining built-ins

- `status`: print the previous foreground command/pipeline's numeric status.
  Zero means success by convention. A pipeline uses its final stage's result.
  A signal termination is represented as 128 plus the signal number, so SIGINT
  is usually 130. The status command itself subsequently succeeds with 0.
- `help`: display the command list and implemented syntax.
- `theme on`: purple background with light text using ANSI color sequences.
- `theme off`: reset the terminal's default color attributes.
- `exit`: leave the shell, sending SIGHUP and SIGCONT to tracked active jobs.
  This is not a persistent job manager. A program that ignores SIGHUP may survive.

Job-management commands, theme, and exit are restricted to standalone foreground
use. A child cannot meaningfully change the parent shell's job table or lifetime.

## 10. Internal data structures

| Structure | Meaning |
|---|---|
| Token | A word or operator, retaining its kind separately from text |
| Redirection | The operator kind and target path |
| Command | NULL-terminated argv list and ordered redirections |
| Pipeline | Commands, foreground/background flag, inspector/profile flags |
| Process | PID, running/stopped/done state, last wait status |
| Job | Process group, processes, display text, timer, saved terminal modes |

Token strings belong to the lexer. Commands point into them during execution.
After launching, forked children have their own address space, so freeing the
parent's tokens does not invalidate child memory. Jobs separately copy their
command line because they may outlive the current main-loop iteration.

## 11. Read the source in this order

1. `main`: setup terminal, read lines, lex, parse, execute, free memory, repeat.
2. `lex`: recognize words/operators while keeping quoted text intact.
3. `parse`: construct commands and reject incomplete syntax before side effects.
4. `builtin_id`, `builtin`: identify custom commands and execute their operations.
5. `redirect`, `run_parent`: configure file descriptors and restore them for
   parent-executed built-ins.
6. `execute`: launch a pipeline, establish its process group, connect descriptors,
   release the launch gate, then wait or return immediately for background work.
7. `foreground`, `record_status`, `reap`: terminal ownership and process lifecycle.
8. `explain`, `release_job`: inspect plans and report completed profiling data.

Supporting functions:

| Function | Why it exists |
|---|---|
| die / allocate | Handle fatal allocation/setup errors consistently |
| set_signal | Install handlers or default/ignored signal dispositions |
| on_interrupt | Set a signal-safe flag without unsafe printing/allocation |
| colors | Apply/reset the theme only when interactive |
| done / stopped | Determine aggregate job state from all its processes |
| exit_code | Decode wait status; raw wait status is not an exit code |
| elapsed | Measure elapsed wall time with a monotonic clock |
| release_job | Report profiling and free a job slot |
| mark_running | Update stopped processes when a job is resumed |
| needs_parent | Enforce the standalone job-control/theme/exit rule |

## 12. Important OS/library calls

| Call | Purpose in qcsh |
|---|---|
| getline | Dynamically allocate/grow the command input buffer |
| fork | Create a child process |
| execvp | Replace a child with the requested external program |
| waitpid | Observe child exits/stops/continues and reap completed children |
| pipe | Create kernel-buffered byte channels |
| dup / dup2 | Copy descriptor references or replace standard input/output |
| open / close | Open/create paths and release descriptors |
| setpgid | Group pipeline processes as one job |
| tcsetpgrp | Choose which process group owns the terminal foreground |
| tcgetattr / tcsetattr | Save/restore terminal settings across jobs |
| sigaction | Set signal dispositions |
| kill | Send a signal; a negative PGID targets a process group |
| clock_gettime | Read a monotonic clock for elapsed timing |
| _exit | End a child without rerunning parent stdio cleanup |

These include both library functions and OS interfaces; they are not all direct
system calls. errno describes a failure only when the relevant function reports
failure. perror turns that error number into readable text.

## 13. Test and publish

Run `make test` before committing. Read test_shell.py for the behavior examples
and test_interactive.py for terminal-control tests using Python's standard pty
module. Sanitizer tests help detect memory/undefined-behavior problems; passing
these tests is not a proof that the shell has no bugs.

From Bash, stage only project files you intend to publish:

```bash
git add main.c README.md GUIDE.md Makefile .gitignore tests .github/workflows/test.yml
git diff --cached
git commit -m "Add parsing, pipelines, job control and execution inspector"
```

Do not add the compiled qcsh binary, local backup, or experiment files. The
.gitignore excludes build outputs. This archive does not include your existing
.git history; preserve that directory when updating your project.
