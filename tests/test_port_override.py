"""port_override: a port's PORT_OVERRIDE definitions match ps2/src units, with or without --long-is-64.

Each case writes a port/src unit and a ps2/src unit under tmp_path and runs the real CLI.
"""

import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parent.parent / 'scripts' / 'port_override.py'
MARKER = 'g_retail_marker'


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def scenario(tmp_path, port_params, ps2_params):
    port = tmp_path / 'port' / 'src'
    write(port / 'a.cpp', f'PORT_OVERRIDE void A::F({port_params}) {{ }}\n')
    overrides = tmp_path / 'overrides.txt'
    listed = run('list', port, '-o', overrides)
    assert listed.returncode == 0, listed.stderr

    ps2 = tmp_path / 'ps2' / 'src'
    write(ps2 / 'a.cpp', f'void A::F({ps2_params}) {{ {MARKER} = 1; }}\n')
    stubs = tmp_path / 'stubs'
    stubs.mkdir()
    return overrides, ps2, stubs


def check(tmp_path, overrides, ps2, stubs, *flags):
    return run('check', '--overrides', overrides, '--stubs', stubs, *flags, ps2, '-o', tmp_path / 'checked')


def strip(tmp_path, overrides, ps2, *flags):
    out = tmp_path / 'stripped.cpp'
    result = run('strip', '--overrides', overrides, *flags, ps2 / 'a.cpp', out)
    assert result.returncode == 0, result.stderr
    return out.read_text(encoding='utf-8')


def test_long_long_port_does_not_match_long_without_option(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'unsigned long long x', 'unsigned long x')

    checked = check(tmp_path, overrides, ps2, stubs)
    assert checked.returncode != 0
    assert 'no ps2/src unit defines it under that signature' in checked.stderr

    assert MARKER in strip(tmp_path, overrides, ps2)


def test_long_long_port_matches_long_with_option(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'unsigned long long x', 'unsigned long x')

    checked = check(tmp_path, overrides, ps2, stubs, '--long-is-64')
    assert checked.returncode == 0, checked.stderr
    assert (tmp_path / 'checked').exists()

    assert MARKER not in strip(tmp_path, overrides, ps2, '--long-is-64')


@pytest.mark.parametrize('ps2_params', [
    'long x',
    'unsigned long x',
    'long int x',
    'const unsigned long *p',
])
def test_long_spellings_in_ps2_match_long_long_with_option(tmp_path, ps2_params):
    port_params = ps2_params.replace('long', 'long long', 1)
    overrides, ps2, stubs = scenario(tmp_path, port_params, ps2_params)

    assert check(tmp_path, overrides, ps2, stubs, '--long-is-64').returncode == 0
    assert MARKER not in strip(tmp_path, overrides, ps2, '--long-is-64')


@pytest.mark.parametrize('port_params, ps2_params', [
    ('int x', 'long x'),
    ('long long x', 'int x'),
])
def test_int_and_long_still_differ_with_option(tmp_path, port_params, ps2_params):
    overrides, ps2, stubs = scenario(tmp_path, port_params, ps2_params)

    checked = check(tmp_path, overrides, ps2, stubs, '--long-is-64')
    assert checked.returncode != 0
    assert 'no ps2/src unit defines it under that signature' in checked.stderr

    assert MARKER in strip(tmp_path, overrides, ps2, '--long-is-64')


@pytest.mark.parametrize('flags', [[], ['--long-is-64']])
def test_same_int_signature_matches_in_both_modes(tmp_path, flags):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')

    assert check(tmp_path, overrides, ps2, stubs, *flags).returncode == 0
    assert MARKER not in strip(tmp_path, overrides, ps2, *flags)
