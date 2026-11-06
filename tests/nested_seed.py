"""Seed from independent opens, including inherited fd paths and overrides."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def seed(data):
    mask = (1 << 64) - 1
    h = 0xcbf29ce484222325 ^ 1
    for byte in data:
        h = ((h ^ byte) * 0x100000001b3) & mask
    h ^= h >> 33
    h = (h * 0xff51afd7ed558ccd) & mask
    h ^= h >> 33
    h = (h * 0xc4ceb9fe1a85ec53) & mask
    return h ^ (h >> 33)


program, emulator = sys.argv[1:]
command = ([part for part in emulator.split(";") if part] + [program])


def check(args, expected, override=None, fds=()):
    env = dict(os.environ, TEAPOT_TEST_SEED=f"{expected:016x}")
    env.pop("TEAPOT_NESTED_SEED", None)
    if override is not None:
        env["TEAPOT_NESTED_SEED"] = override
    result = subprocess.run(command + args, env=env, pass_fds=fds, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert f"seed={expected:016x}" in result.stdout
    print(f"{args}: seed={expected:016x}")


with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "input"
    data = bytes(range(256)) * 513
    path.write_bytes(data)
    check([], seed(b""))
    check([str(path) + ".missing"], seed(b""))
    check([str(path)], seed(data))
    fd = os.open(path, os.O_RDONLY)
    try:
        os.lseek(fd, 7, os.SEEK_SET)
        check([f"/dev/fd/{fd}"], seed(data), fds=(fd,))
        assert os.lseek(fd, 0, os.SEEK_CUR) == 7, "seed read consumed the program's descriptor"
    finally:
        os.close(fd)
    check([str(path)], 0xfedcba9876543210, "0xfedcba9876543210")
    check([], 0, "0")
