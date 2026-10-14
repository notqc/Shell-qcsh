"""Linux PTY integration test: a real controlling terminal, no third-party packages."""
import errno
import os
import pathlib
import pty
import re
import select
import signal
import tempfile
import time

BIN = str(pathlib.Path(__file__).resolve().parents[1] / 'qcsh')

def main():
    with tempfile.TemporaryDirectory() as directory:
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(directory)
            os.execv(BIN, [BIN])
        pending = b''
        def send(text):
            os.write(fd, text.encode())
        def expect(pattern, timeout=5):
            nonlocal pending
            deadline = time.monotonic() + timeout
            regex = re.compile(pattern)
            while time.monotonic() < deadline:
                match = regex.search(pending)
                if match:
                    result = pending[:match.end()]
                    pending = pending[match.end():]
                    return result
                ready, _, _ = select.select([fd], [], [], max(0, deadline-time.monotonic()))
                if ready:
                    try:
                        chunk = os.read(fd, 65536)
                    except OSError as exc:
                        raise AssertionError(f'Unexpected terminal EOF: {pending!r}') from exc
                    if not chunk: break
                    pending += chunk
            raise AssertionError(f'Missing {pattern!r}; output={pending!r}')
        try:
            expect(b'qcsh> ')
            send('theme off\n'); expect(b'qcsh> ')
            send('sleep 30\n')
            time.sleep(0.1)
            send('\x03'); expect(b'qcsh> ')
            send('status\n'); expect(b'130\r\n'); expect(b'qcsh> ')
            send('sleep 30\n')
            time.sleep(0.1)
            send('\x1a')
            output = expect(b'qcsh> ')
            job = re.search(rb'\[(\d+)\] Stopped', output).group(1).decode()
            send('jobs\n'); expect(b'Stopped'); expect(b'qcsh> ')
            send(f'bg {job}\n'); expect(b'Running'); expect(b'qcsh> ')
            send(f'fg {job}\n')
            time.sleep(0.1)
            send('\x03'); expect(b'qcsh> ')
            # cat must receive the terminal before attempting to read it.
            for _ in range(5):
                send('cat\n'); time.sleep(0.03)
                send('hello-terminal\n'); expect(b'hello-terminal\r\nhello-terminal\r\n')
                send('\x04'); expect(b'qcsh> ')
            send('sleep 0.1 &\n'); expect(b'qcsh> ')
            time.sleep(0.2)
            send('jobs\n'); output = expect(b'qcsh> ')
            assert b'Done' in output, output
            send('\x03'); expect(b'qcsh> ')
            send('printf alive\n'); expect(b'aliveqcsh> ')
            send('exit\n')
            _, status = os.waitpid(pid, 0)
            assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status
            print('PASS: Ctrl+C, Ctrl+Z, jobs, bg, fg, terminal input, background reaping, prompt recovery')
        finally:
            os.close(fd)
            try: os.kill(pid, signal.SIGKILL)
            except ProcessLookupError: pass
            try: os.waitpid(pid, 0)
            except ChildProcessError: pass

if __name__ == '__main__': main()
