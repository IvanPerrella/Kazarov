"""kz-lock: waiting, exit codes, signal forwarding."""

import os
import signal
import subprocess
import sys
import time
from pathlib import Path

BIN = Path(__file__).resolve().parent.parent / 'bin'
KZ_LOCK = BIN / 'kz-lock'


def _kz_lock(*args, **kwargs):
    return subprocess.Popen([sys.executable, str(KZ_LOCK), *args], **kwargs)


def test_runs_command_and_returns_its_exit_code(tmp_path):
    lock = tmp_path / 'a.lock'
    result = subprocess.run([sys.executable, str(KZ_LOCK), '--lock', str(lock), '--',
                             sys.executable, '-c', 'print("ran")'],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout == 'ran\n'
    assert lock.exists()

    result = subprocess.run([sys.executable, str(KZ_LOCK), '--lock', str(lock), '--',
                             sys.executable, '-c', 'import sys; sys.exit(7)'])
    assert result.returncode == 7


def test_creates_parent_directories(tmp_path):
    lock = tmp_path / '.kazarov' / 'locks' / 'ps2-pal.lock'
    result = subprocess.run([sys.executable, str(KZ_LOCK), '--lock', str(lock), '--', 'true'])
    assert result.returncode == 0
    assert lock.is_file()


def test_second_holder_waits_and_prints_the_message_once(tmp_path):
    lock = tmp_path / 'held.lock'
    marker = tmp_path / 'released'
    holder = subprocess.Popen(
        [sys.executable, '-c',
         'import fcntl, os, sys, time\n'
         'fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT)\n'
         'fcntl.flock(fd, fcntl.LOCK_EX)\n'
         'print("held", flush=True)\n'
         'time.sleep(1.0)\n'
         'open(sys.argv[2], "w").close()\n',
         str(lock), str(marker)],
        stdout=subprocess.PIPE, text=True)
    try:
        assert holder.stdout.readline().strip() == 'held'
        start = time.monotonic()
        result = subprocess.run(
            [sys.executable, str(KZ_LOCK), '--lock', str(lock), '--message', 'build dir busy', '--',
             sys.executable, '-c', 'import os, sys; sys.exit(0 if os.path.exists(sys.argv[1]) else 3)',
             str(marker)],
            capture_output=True, text=True)
        waited = time.monotonic() - start
    finally:
        holder.wait(timeout=10)

    assert result.returncode == 0, result.stderr
    assert waited >= 0.5
    assert result.stderr.count(f'kazarov: waiting for {lock}') == 1
    assert result.stderr.count('kazarov: build dir busy') == 1


def test_uncontended_lock_prints_nothing(tmp_path):
    result = subprocess.run([sys.executable, str(KZ_LOCK), '--lock', str(tmp_path / 'x.lock'),
                             '--message', 'busy', '--', 'true'], capture_output=True, text=True)
    assert result.returncode == 0
    assert 'waiting' not in result.stderr
    assert 'busy' not in result.stderr


def test_sigterm_is_forwarded_to_the_command(tmp_path):
    proc = subprocess.Popen(
        [sys.executable, str(KZ_LOCK), '--lock', str(tmp_path / 's.lock'), '--',
         sys.executable, '-c', 'import time; print("ready", flush=True); time.sleep(60)'],
        stdout=subprocess.PIPE, text=True)
    try:
        assert proc.stdout.readline().strip() == 'ready'
        proc.send_signal(signal.SIGTERM)
        code = proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
    assert code == 128 + signal.SIGTERM
