#!/usr/bin/env python3
"""Supervisor integration test: real process and TCP channel.

It does not start the real app: it uses a fake bootloader (`exec sleep 30`)
and a temporary `flash.bin`. Covers states, power on/off/reset, bootloader,
crash, factory reset and the optional token.
"""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TOKEN = "test-token"


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def send(port: int, command: str, token: str | None = TOKEN) -> dict:
    line = f"{token} {command}" if token else command
    with socket.create_connection(("127.0.0.1", port), 2.0) as sock:
        sock.settimeout(2.0)
        sock.sendall((line + "\n").encode("ascii"))
        data = b""
        while b"\n" not in data:
            chunk = sock.recv(256)
            if not chunk:
                break
            data += chunk
    if not data:
        raise AssertionError(f"sin respuesta a {command}")
    return json.loads(data.split(b"\n")[0])


def wait_state(port: int, state: str, timeout: float = 4.0) -> dict:
    deadline = time.time() + timeout
    last: dict = {}
    while time.time() < deadline:
        try:
            last = send(port, "STATUS")
            if last.get("state") == state:
                return last
        except OSError:
            pass
        time.sleep(0.05)
    raise AssertionError(f"no llego a {state}; ultimo={last}")


def wait_port(port: int, timeout: float = 4.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("el supervisor no abrio el puerto")


def main() -> int:
    if len(sys.argv) < 2:
        print("uso: test_supervisor_control.py <supervisor-bin>", file=sys.stderr)
        return 2

    supervisor = sys.argv[1]
    workdir = Path(tempfile.mkdtemp(prefix="lamp-supervisor-"))
    fake_boot = workdir / "fake-bootloader"
    fake_boot.write_text("#!/bin/sh\nexec sleep 30\n")
    fake_boot.chmod(0o755)
    flash = workdir / "flash.bin"
    flash.write_bytes(b"x" * 1024)
    port = free_port()
    log_path = workdir / "supervisor.log"

    env = os.environ.copy()
    env.update(
        {
            "BOOTLOADER_BIN": str(fake_boot),
            "FLASH_PATH": str(flash),
            "SUPERVISOR_PORT": str(port),
            "SUPERVISOR_TOKEN": TOKEN,
        }
    )

    log_file = open(log_path, "w", encoding="utf-8")  # noqa: SIM115
    process = subprocess.Popen(
        [supervisor],
        env=env,
        stdout=log_file,
        stderr=subprocess.STDOUT,
    )

    try:
        wait_port(port)

        # Automatic boot.
        status = wait_state(port, "APP_RUNNING")
        pid = int(status["pid"])
        assert pid > 0, status

        # Token required.
        unauthorized = send(port, "STATUS", token=None)
        assert unauthorized.get("error") == "UNAUTHORIZED", unauthorized

        # Power off / on.
        off = send(port, "POWER_OFF")
        assert off["state"] == "POWERED_OFF", off
        assert wait_state(port, "POWERED_OFF")["pid"] == 0
        on = send(port, "POWER_ON")
        assert on["state"] == "BOOTING", on
        wait_state(port, "APP_RUNNING")

        # Reset (the PID changes).
        pid_before = wait_state(port, "APP_RUNNING")["pid"]
        send(port, "RESET")
        time.sleep(0.4)
        status = wait_state(port, "APP_RUNNING")
        assert status["pid"] != pid_before, (pid_before, status)

        # Persistent bootloader and exit.
        boot = send(port, "ENTER_BOOTLOADER")
        assert boot["state"] == "BOOTLOADER_MODE", boot
        assert wait_state(port, "BOOTLOADER_MODE")["forced"] is True
        send(port, "POWER_ON")
        wait_state(port, "APP_RUNNING")

        # Unexpected crash -> FAULT and it stays powered off.
        pid = int(wait_state(port, "APP_RUNNING")["pid"])
        os.kill(pid, signal.SIGKILL)
        fault = wait_state(port, "FAULT")
        assert fault["pid"] == 0, fault
        assert fault["last_exit"] != -1, fault
        send(port, "POWER_ON")
        wait_state(port, "APP_RUNNING")

        # Factory reset: deletes flash.bin and boots again.
        assert flash.exists()
        factory = send(port, "FACTORY_RESET")
        assert factory["state"] in ("BOOTING", "APP_RUNNING"), factory
        wait_state(port, "APP_RUNNING")
        assert not flash.exists(), "factory reset no borro flash.bin"

        print("OK: supervisor (estados, power, reset, bootloader, crash, "
              "factory reset y token)")
        return 0
    finally:
        try:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        log_file.close()
        if process.returncode not in (0, -signal.SIGTERM):
            print(f"supervisor exit={process.returncode}", file=sys.stderr)
            print(log_path.read_text(encoding="utf-8"), file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
