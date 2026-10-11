"""host/kazarov.ps1 dispatch and container argv, run under pwsh against a scratch project.

A fake docker records its argv, and a fake Windows host script records its argv. Skipped
when no pwsh is found: KZ_PWSH names one, otherwise pwsh is taken from PATH.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
KZ = HERE.parent
SCRIPT = KZ / "host" / "kazarov.ps1"
IMAGE_ID = "sha256:" + "ab" * 32

CONF = """\
KZ_PROJECT=scratch
KZ_DEFAULT_BUILD=ps2
KZ_DEFAULT_BUILD_WINDOWS=win
KZ_DEFAULT_RUN=ps2
KZ_DEFAULT_RUN_WINDOWS=win
KZ_WINDOWS_HOST_SCRIPT=fake_host.ps1
KZ_WINDOWS_CONFIGS=(win)
KZ_CONTAINER_CONFIGS="ps2"
"""

FAKE_HOST = """\
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'windows-argv.json'), (ConvertTo-Json -InputObject @($args) -Compress))
"""

FAKE_DOCKER = """\
#!/usr/bin/env python3
import json
import os
import sys

with open(os.environ["FAKE_DOCKER_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
if sys.argv[1:3] == ["image", "inspect"]:
    print("%s")
sys.exit(0)
""" % IMAGE_ID


def find_pwsh():
    configured = os.environ.get("KZ_PWSH")
    if configured:
        return configured if Path(configured).is_file() else None
    return shutil.which("pwsh")


@pytest.fixture
def pwsh():
    found = find_pwsh()
    if found is None:
        pytest.skip("pwsh not found: set KZ_PWSH or put pwsh on PATH")
    return found


@pytest.fixture
def project(tmp_path):
    root = tmp_path / "my project"
    root.mkdir()
    (root / "kazarov.conf").write_text(CONF, encoding="utf-8")
    (root / "fake_host.ps1").write_text(FAKE_HOST, encoding="utf-8")
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    docker = bin_dir / "docker"
    docker.write_text(FAKE_DOCKER, encoding="utf-8")
    docker.chmod(0o755)
    return root, bin_dir


def run_kazarov(pwsh, project, *args):
    root, bin_dir = project
    log = bin_dir.parent / "docker.log"
    log.unlink(missing_ok=True)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([str(bin_dir), str(Path(pwsh).resolve().parent), env["PATH"]])
    env["BUILDER"] = "docker"
    env["FAKE_DOCKER_LOG"] = str(log)
    env.pop("REBUILD_IMAGE", None)
    result = subprocess.run([pwsh, "-NoProfile", "-File", str(SCRIPT), str(root), *args],
                            capture_output=True, text=True, env=env, cwd=str(bin_dir))
    docker_calls = []
    if log.exists():
        docker_calls = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
    windows = root / "windows-argv.json"
    windows_argv = json.loads(windows.read_text(encoding="utf-8")) if windows.exists() else None
    if windows.exists():
        windows.unlink()
    return result, docker_calls, windows_argv


def container_run(calls):
    runs = [call for call in calls if call[:1] == ["run"]]
    assert len(runs) == 1, calls
    return runs[0]


def test_no_config_uses_the_windows_default(pwsh, project):
    result, calls, windows_argv = run_kazarov(pwsh, project, "build")
    assert result.returncode == 0, result.stderr
    assert windows_argv == ["build", "win"]
    assert calls == []


def test_listed_container_config_runs_in_the_container(pwsh, project):
    result, calls, windows_argv = run_kazarov(pwsh, project, "build", "ps2")
    assert result.returncode == 0, result.stderr
    assert windows_argv is None
    run = container_run(calls)
    assert run[-4:] == ["tools/kazarov/bin/kazarov", "--inside", "build", "ps2"]
    assert IMAGE_ID in run
    assert f"{project[0]}:/work/scratch" in run


def test_unlisted_argument_is_forwarded_to_the_default_config(pwsh, project):
    result, calls, windows_argv = run_kazarov(pwsh, project, "build", "foo")
    assert result.returncode == 0, result.stderr
    assert windows_argv == ["build", "win", "foo"]
    assert calls == []


def test_dash_argument_is_forwarded_to_the_default_config(pwsh, project):
    result, calls, windows_argv = run_kazarov(pwsh, project, "build", "-x")
    assert result.returncode == 0, result.stderr
    assert windows_argv == ["build", "win", "-x"]
    assert calls == []


def test_argument_with_metacharacters_reaches_docker_intact(pwsh, project):
    tricky = 'foo&echo X&rem "q" bar baz'
    result, calls, windows_argv = run_kazarov(pwsh, project, "build", "ps2", tricky)
    assert result.returncode == 0, result.stderr
    assert windows_argv is None
    run = container_run(calls)
    assert run[-6:] == [IMAGE_ID, "tools/kazarov/bin/kazarov", "--inside", "build", "ps2", tricky]
