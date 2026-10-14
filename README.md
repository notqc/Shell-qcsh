# qcsh

An educational Linux shell in C, built from a basic command interpreter. Includes
custom file commands, quote-aware parsing, external processes, pipelines,
redirection, foreground/background jobs, and an execution inspector.

## Build (Linux or Ubuntu/WSL)

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -g main.c -o qcsh
./qcsh
```

Or use `make`. GCC and Make are available through Ubuntu's `build-essential`
package. Python 3 is required only for tests. No Readline dependency.

Read **GUIDE.md** for setup, every command, architecture, and function explanations.

## Commands

| Command | Purpose |
|---|---|
| `createFile "notes.txt"` | Create a new empty file; fail if already present |
| `createFile --overwrite "notes.txt"` | Explicitly create or truncate a file |
| `createFolder "my folder"` | Create one directory (parents must exist) |
| `openFolder "my folder"` | Change working directory |
| `cd ..` | Alias for openFolder; no argument goes to HOME |
| `openFile "notes.txt"` | Print file contents; does not start an editor |
| `pwd` | Show current working directory |
| `help` | List supported syntax and commands |
| `status` | Print the preceding command's status |
| `jobs` | Show active/stopped jobs |
| `fg 1` | Foreground job 1; resume it if stopped |
| `bg 1` | Continue job 1 in the background |
| `theme on` / `theme off` | Enable/disable the purple theme |
| `exit` | Exit and send SIGHUP/SIGCONT to active jobs |
| `explain COMMAND` | Describe a pipeline without executing it |
| `profile COMMAND` | Execute; report elapsed wall time and per-stage PID/status |

External programs are found through `execvp` and PATH. Examples: `ls -l`,
`printf`, `grep`, `wc`, `sleep`, `gcc`, `python3`. Those programs must be installed.

## Syntax

- Single/double quotes and escapes, including empty arguments.
- Multiple pipeline stages: `cat notes.txt | grep OS | wc -l`.
- Standard input/output redirection: `<`, `>`, `>>`.
- Trailing `&` to launch a background job.
- Ctrl+C interrupts a foreground process group; Ctrl+Z stops it.
- Ctrl+D at an empty prompt exits. No line-editing/history library is used.
- Quote paths containing spaces: `openFolder "My Documents"`.

## Tests

```bash
make test
make sanitize
```

`make test` runs 12 behavioral test methods plus a Linux pseudo-terminal test
covering Ctrl+C, Ctrl+Z, jobs, bg, fg, terminal readers, background reaping, and
prompt recovery. Tests use temporary directories. A GitHub Actions workflow is
included; it has not been run on your GitHub repository.

Local validation: warning-free GCC build, behavioral and PTY tests passed.
AddressSanitizer and UndefinedBehaviorSanitizer tests passed with
`ASAN_OPTIONS=detect_leaks=0 make sanitize`. LeakSanitizer could not inspect
processes in the execution environment, so leak freedom is not claimed. On a
normal Linux host, try `make sanitize` with its default leak detection first.

## Scope and limitations

This is a learning implementation, not a Bash/POSIX-compatible replacement.
No variable or tilde expansion, globbing, command substitution, semicolon
sequences, `&&`, `||`, shell functions, loops, heredocs, `2>`, or descriptor
merging. Unsupported punctuation may be treated as literal arguments: do not
paste Bash scripts into qcsh. One supported pipeline per input line.

Limits: 512 tokens, 32 pipeline stages, 128 argv entries per stage including the
program name, 32 redirections per stage, 64 tracked jobs. Input lines use
`getline`; the fixed limits are checked, not silently truncated.

Jobs are reaped before prompts and when job commands execute. A child finishing
while the shell is idle can remain a zombie until the next input; asynchronous
job notifications/self-pipe event-loop integration are future work. Background
children are also collected when their status is inspected. No daemonization or
persistence guarantee: active jobs receive SIGHUP/SIGCONT on exit, but a process
that ignores SIGHUP may survive. No guarantee of killing detached descendants.

Standalone built-ins run in the shell. Ordinary built-ins in pipelines or with
`&` run in children, so `openFolder` there cannot change the parent directory.
`exit`, `jobs`, `fg`, `bg`, and `theme` must be standalone foreground commands.

`profile` reports wall-clock time, not CPU time or peak memory. It includes time
spent stopped; background measurements are reported when completion is reaped.
The pipeline's status is the last stage's status, without pipefail.

The theme is emitted only for interactive input/output. It resets on normal
exit; external programs or abrupt termination can affect terminal appearance.
This shell does not redraw a full-screen UI or implement persistent history.

## Project files

- `main.c`: source, organized into functions with comments.
- `GUIDE.md`: explanation and walkthrough.
- `Makefile`: build/test/sanitizer/clean targets.
- `tests/`: behavioral and interactive tests.
- `.github/workflows/test.yml`: CI configuration.
- `.gitignore`: excludes generated executables and object files.

Compiled binaries and trial files are not included in the source archive.

## Reference and provenance

Project started from studying the GeeksforGeeks tutorial:
https://www.geeksforgeeks.org/c/making-linux-shell-c/

This expanded implementation and its tests were developed with AI assistance.
Understand and modify the implementation before presenting it as your project;
claim only features and measurements you have verified.
