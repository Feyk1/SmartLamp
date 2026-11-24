"""Supervisor control channel client (line-based TCP)."""

from __future__ import annotations

import json
import socket


class SupervisorError(Exception):
    """The supervisor does not respond or rejected the command."""


class SupervisorClient:
    def __init__(
        self,
        host: str,
        port: int,
        token: str | None = None,
        timeout: float = 6.0,
    ) -> None:
        self.host = host
        self.port = port
        self.token = token or None
        self.timeout = timeout

    def command(self, command: str) -> dict:
        """Sends a command and returns the supervisor's JSON response."""
        line = f"{self.token} {command}" if self.token else command
        try:
            with socket.create_connection(
                (self.host, self.port), self.timeout
            ) as sock:
                sock.settimeout(self.timeout)
                sock.sendall((line + "\n").encode("ascii"))
                data = b""
                while b"\n" not in data:
                    chunk = sock.recv(256)
                    if not chunk:
                        break
                    data += chunk
        except OSError as exc:
            raise SupervisorError(f"supervisor unreachable: {exc}") from exc

        if not data:
            raise SupervisorError("empty response from supervisor")
        try:
            result = json.loads(data.split(b"\n")[0])
        except json.JSONDecodeError as exc:
            raise SupervisorError("non-JSON response from supervisor") from exc
        if not isinstance(result, dict):
            raise SupervisorError("unexpected response from supervisor")
        if "error" in result:
            raise SupervisorError(str(result["error"]))
        return result

    def status(self) -> dict:
        return self.command("STATUS")
