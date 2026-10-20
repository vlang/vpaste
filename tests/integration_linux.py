#!/usr/bin/env python3
"""Exercise the real hotkey/click/clipboard/type flow on an isolated X server."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'dist/keypour'
BASE = dict(hotkey='Ctrl+Alt+V', interval_ms=2, key_hold_ms=2,
            start_delay_ms=150, line_delay_ms=5, arm_timeout_ms=4000,
            max_chars=100000, input_mode='keys', trim_trailing_newlines=False)


def wait_for(test, message, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = test()
        if result:
            return result
        time.sleep(0.01)
    raise AssertionError(message)


class Desktop:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.processes = []
        self.app = None
        self.clip = None
        self.env = dict(os.environ, XDG_SESSION_TYPE='x11', LC_ALL='C.UTF-8')
        self.env.pop('WAYLAND_DISPLAY', None)
        self.env['XDG_CONFIG_HOME'] = str(self.folder)
        read_fd, write_fd = os.pipe()
        self.server = subprocess.Popen(
            ['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1024x768x24', '-nolisten', 'tcp'],
            pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=open(self.folder / 'xvfb.log', 'wb'))
        self.processes.append(self.server)
        os.close(write_fd)
        with os.fdopen(read_fd) as output:
            number = output.readline().strip()
        if not number:
            self.server.wait(timeout=2)
            raise RuntimeError((self.folder / 'xvfb.log').read_text()[-3000:])
        self.env['DISPLAY'] = ':' + number
        self.receivers = []
        for i, x in enumerate((100, 500)):
            logfile = self.folder / f'received-{i}.txt'
            proc = subprocess.Popen([str(ROOT / 'build/x11_receiver'), str(logfile), str(x), f'KeypourTest{i}'],
                                    env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.processes.append(proc)
            window = proc.stdout.readline().strip()
            if not window:
                raise RuntimeError('X11 test receiver did not start: ' + proc.stderr.read())
            self.receivers.append((window, logfile, x + 80))
        self.click(0)

    def command(self, *args):
        return subprocess.run(args, env=self.env, capture_output=True, check=True, text=True, timeout=5)

    def hotkey(self, combination='ctrl+alt+v'):
        self.command('xdotool', 'key', '--delay', '8', combination)

    def click(self, which=0):
        self.command('xdotool', 'mousemove', str(self.receivers[which][2]), '180', 'click', '1')
        time.sleep(0.03)

    def focus(self, which):
        self.command('xdotool', 'windowfocus', '--sync', self.receivers[which][0])

    def received(self, which=0):
        return self.receivers[which][1].read_bytes()

    def clear(self):
        for _, path, _ in self.receivers:
            path.write_bytes(b'')

    def set_clipboard(self, value):
        if self.clip and self.clip.poll() is None:
            self.clip.terminate()
            self.clip.wait(timeout=2)
        self.clip = subprocess.Popen(['xclip', '-selection', 'clipboard', '-in', '-quiet'], env=self.env,
                                     stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.processes.append(self.clip)
        self.clip.stdin.write(value.encode('utf-8') if isinstance(value, str) else value)
        self.clip.stdin.close()
        # Wait for the owner to actually offer the requested data.
        wait_for(lambda: subprocess.run(['xclip', '-selection', 'clipboard', '-out'], env=self.env,
                                        capture_output=True, timeout=2).stdout == (value.encode() if isinstance(value, str) else value),
                 'test clipboard owner did not become ready')

    def start(self, **overrides):
        self.stop()
        self.clear()
        self.click(0)
        config = dict(BASE, **overrides)
        self.config = self.folder / 'config.json'
        self.config.write_text(json.dumps(config))
        self.log_path = self.folder / 'keypour.log'
        self.log_handle = self.log_path.open('wb')
        self.app = subprocess.Popen([str(BINARY), '--config', str(self.config)], env=self.env,
                                    stdout=self.log_handle, stderr=subprocess.STDOUT)
        self.processes.append(self.app)
        wait_for(lambda: 'Copy text,' in self.log() or self.app.poll() is not None,
                 'application failed to announce readiness')
        assert self.app.poll() is None, self.log()

    def log(self):
        return self.log_path.read_text(errors='replace')

    def wait_log(self, text, timeout=4):
        wait_for(lambda: text in self.log(), f'Expected status {text!r}. Actual:\n{self.log()}', timeout)

    def stop(self):
        if self.app and self.app.poll() is None:
            self.app.send_signal(signal.SIGTERM)
            self.app.wait(timeout=3)
            assert self.app.returncode == 0, 'SIGTERM was not handled gracefully'
        if getattr(self, 'log_handle', None):
            self.log_handle.close()
        self.app = None

    def close(self):
        self.stop()
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)


def test_all(d):
    passed = []

    d.start()
    text = 'Aa09 !@#$%^&*()_+-=[]{};:\'",.<>/?\\|`~\tX\r\nY\rZ\n'
    d.set_clipboard(text)
    d.hotkey()
    d.wait_log('Armed:')
    time.sleep(0.25)
    assert d.received() == b'', 'typed before selecting the target'
    d.click()
    d.wait_log('Finished:')
    expected = text.replace('\r\n', '\n').replace('\r', '\n').encode()
    assert d.received() == expected, (d.received(), expected, d.log())
    passed.append('literal punctuation, CRLF/CR, Tab, trailing Enter, click gate')

    d.start(hotkey='Ctrl+Shift+F8', trim_trailing_newlines=True)
    d.set_clipboard('first snapshot\n\n')
    d.hotkey('ctrl+shift+F8')
    d.wait_log('Armed:')
    d.set_clipboard('changed clipboard')
    d.click()
    d.wait_log('Finished:')
    assert d.received() == b'first snapshot', (d.received(), d.log())
    passed.append('custom hotkey, snapshot timing, optional trailing newline removal')

    d.start()
    d.set_clipboard('must not be typed')
    d.hotkey()
    d.wait_log('Armed:')
    d.hotkey('Escape')
    d.wait_log('Cancelled.')
    d.click()
    time.sleep(0.25)
    assert d.received() == b'', (d.received(), d.log())
    passed.append('Escape cancels and is consumed')

    for locks in [('Caps_Lock',), ('Num_Lock',), ('Caps_Lock', 'Num_Lock')]:
        d.start()
        try:
            for key in locks:
                d.hotkey(key)
            d.set_clipboard('must not be typed')
            d.hotkey()
            d.wait_log('Armed:')
            d.hotkey('Escape')
            d.wait_log('Cancelled.')
            d.click()
            time.sleep(0.1)
            assert d.received() == b'', (locks, d.received(), d.log())
        finally:
            for key in reversed(locks):
                d.hotkey(key)
        passed.append('Escape cancels and is consumed with ' + ' + '.join(locks))

    d.start()
    d.set_clipboard('must not be typed')
    d.hotkey()
    d.wait_log('Armed:')
    d.hotkey()
    d.wait_log('Cancelled.')
    d.click()
    time.sleep(0.2)
    assert d.received() == b'', (d.received(), d.log())
    passed.append('second hotkey cancels')

    d.start()
    d.set_clipboard('prefix\x1bsuffix')
    d.hotkey()
    d.wait_log('Not armed:')
    d.click()
    time.sleep(0.2)
    assert d.received() == b'', d.received()
    passed.append('control-byte rejection before any key')

    d.start()
    d.set_clipboard('prefix🙂')
    d.hotkey()
    d.wait_log('Armed:')
    d.click()
    d.wait_log('Cancelled before typing:')
    assert d.received() == b'', (d.received(), d.log())
    passed.append('whole-text preflight prevents a partial unsupported paste')

    d.start()
    d.hotkey('Caps_Lock')
    try:
        d.set_clipboard('aAzZ09!')
        d.hotkey()
        d.wait_log('Armed:')
        d.click()
        d.wait_log('Finished:')
        assert d.received() == b'aAzZ09!', (d.received(), d.log())
    finally:
        d.hotkey('Caps_Lock')
    passed.append('Caps Lock mapping and preservation')

    d.start(interval_ms=30)
    d.set_clipboard('A' * 100)
    d.hotkey()
    d.wait_log('Armed:')
    d.click()
    wait_for(lambda: len(d.received()) >= 4, 'no prefix was typed')
    d.focus(1)
    d.wait_log('Stopped: focused window')
    time.sleep(0.15)
    assert 4 <= len(d.received()) < 100
    assert d.received(1) == b'', ('keystrokes reached a different window', d.received(1))
    passed.append('focus change stops an in-progress paste')

    d.start(arm_timeout_ms=1000)
    d.set_clipboard('timeout')
    d.hotkey()
    d.wait_log('Selection timed out.', 3)
    d.click()
    assert d.received() == b''
    passed.append('armed selection expires without typing')

    d.start(max_chars=200000)
    d.set_clipboard('x' * 100000)
    d.hotkey()
    d.wait_log('Armed: 100000 characters.')
    d.hotkey('Escape')
    d.wait_log('Cancelled.')
    assert d.received() == b''
    passed.append('large ICCCM clipboard/INCR capture and cancellation')

    d.stop()
    for name in passed:
        print('PASS', name)
    print(f'PASS {len(passed)} end-to-end X11 scenarios')


if __name__ == '__main__':
    if not BINARY.is_file():
        raise SystemExit('Build the application before running this test: scripts/build.sh')
    with tempfile.TemporaryDirectory(prefix='x11-integration-', dir=ROOT / 'build') as folder:
        desktop = None
        try:
            desktop = Desktop(folder)
            test_all(desktop)
        except Exception:
            if desktop and getattr(desktop, 'log_path', None):
                print(desktop.log(), file=sys.stderr)
            raise
        finally:
            if desktop:
                desktop.close()
