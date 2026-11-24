"""Minimal Fly.io Machines API client.

It only covers what the orchestrator needs: create/delete ephemeral Apps,
create/wait/delete Machines, and list Apps to clean up orphans.

Reference: https://fly.io/docs/machines/api/
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import httpx

from src import settings


class MachinesApiError(Exception):
    """The Fly API rejected the operation or is not configured."""


@dataclass
class Machine:
    """Minimal data of a newly created Machine."""

    id: str
    app: str
    state: str = "created"
    private_ip: str | None = None


class MachinesApi:
    """Asynchronous Machines API client.

    `client` allows injecting an `httpx.AsyncClient` with `MockTransport` in
    the tests; in production it is built with the organization token.
    """

    def __init__(
        self,
        token: str | None = None,
        base_url: str | None = None,
        client: httpx.AsyncClient | None = None,
        timeout_s: float = 20.0,
    ) -> None:
        self.token = token if token is not None else settings.FLY_ORG_TOKEN
        self.base_url = (base_url or settings.FLY_API_URL).rstrip("/")
        if client is not None:
            self._client = client
        else:
            if not self.token:
                raise MachinesApiError(
                    "FLY_ORG_TOKEN not configured; cannot orchestrate"
                )
            self._client = httpx.AsyncClient(
                base_url=self.base_url,
                headers={"Authorization": f"Bearer {self.token}"},
                timeout=timeout_s,
            )

    async def __aenter__(self) -> MachinesApi:
        return self

    async def __aexit__(self, *_exc: object) -> None:
        await self.aclose()

    async def aclose(self) -> None:
        await self._client.aclose()

    async def _request(
        self, method: str, path: str, payload: dict[str, Any] | None = None
    ) -> Any:
        try:
            response = await self._client.request(method, path, json=payload)
        except httpx.HTTPError as exc:
            raise MachinesApiError(f"Machines API unreachable: {exc}") from exc

        if response.status_code >= 400:
            detail = response.text[:200]
            try:
                body = response.json()
                detail = str(body.get("error", detail))
            except ValueError:
                pass
            raise MachinesApiError(
                f"Machines API {response.status_code}: {detail}"
            )
        if response.status_code == 204 or not response.content:
            return None
        try:
            return response.json()
        except ValueError as exc:
            raise MachinesApiError("non-JSON response from Machines API") from exc

    async def list_apps(self) -> list[str]:
        """Names of the organization's Apps."""
        data = await self._request(
            "GET", f"/apps?org_slug={settings.FLY_ORG_SLUG}"
        )
        apps = data.get("apps", []) if isinstance(data, dict) else []
        return [
            str(app["name"])
            for app in apps
            if isinstance(app, dict) and "name" in app
        ]

    async def create_app(self, app_name: str) -> None:
        """Creates the App; if it already exists (409) it reuses it."""
        try:
            await self._request(
                "POST",
                "/apps",
                {"app_name": app_name, "org_slug": settings.FLY_ORG_SLUG},
            )
        except MachinesApiError as exc:
            if "409" not in str(exc):
                raise

    async def delete_app(self, app_name: str) -> None:
        """Deletes the App and its Machines in cascade (404 is ignored)."""
        try:
            await self._request("DELETE", f"/apps/{app_name}")
        except MachinesApiError as exc:
            if "404" not in str(exc):
                raise

    async def create_machine(
        self, app_name: str, config: dict[str, Any]
    ) -> Machine:
        data = await self._request(
            "POST", f"/apps/{app_name}/machines", config
        )
        if not isinstance(data, dict) or "id" not in data:
            raise MachinesApiError("response without id when creating Machine")
        return Machine(
            id=str(data["id"]),
            app=app_name,
            state=str(data.get("state", "created")),
            private_ip=data.get("private_ip"),
        )

    async def wait_started(
        self, machine: Machine, timeout_s: int = 30
    ) -> Machine:
        """Waits for the Machine to start and returns its private IP."""
        data = await self._request(
            "GET",
            f"/apps/{machine.app}/machines/{machine.id}/wait"
            f"?state=started&timeout={timeout_s}",
        )
        if isinstance(data, dict):
            machine.state = str(data.get("state", machine.state))
            machine.private_ip = data.get("private_ip") or machine.private_ip
        if not machine.private_ip:
            machine.private_ip = await self._private_ip(machine)
        if not machine.private_ip:
            raise MachinesApiError("Machine has no private IP (6PN)")
        return machine

    async def _private_ip(self, machine: Machine) -> str | None:
        data = await self._request(
            "GET", f"/apps/{machine.app}/machines/{machine.id}"
        )
        if not isinstance(data, dict):
            return None
        return data.get("private_ip")

    async def delete_machine(self, app_name: str, machine_id: str) -> None:
        try:
            await self._request(
                "DELETE", f"/apps/{app_name}/machines/{machine_id}?force=true"
            )
        except MachinesApiError as exc:
            if "404" not in str(exc):
                raise
