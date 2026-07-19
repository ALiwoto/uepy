"""Client wrapper for Epic's live Unreal Python remote execution protocol."""

from __future__ import annotations

import importlib.util
import json
import os
import time
import uuid
from pathlib import Path
from types import ModuleType
from typing import Any, Iterable

from .errors import DiscoveryError, ProtocolError, RemoteCommandError, UepyError
from .locator import find_remote_execution


def _load_remote_module(path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location("_uepy_epic_remote_execution", path)
    if spec is None or spec.loader is None:
        raise UepyError(f"Could not load Epic remote execution helper: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _paths_overlap(left: str, right: str) -> bool:
    try:
        left_path = Path(left).resolve()
        right_path = Path(right).resolve()
        return left_path == right_path or left_path in right_path.parents or right_path in left_path.parents
    except (OSError, ValueError):
        return False


class UnrealRemoteClient:
    """Discover, select, and query a running Unreal Editor instance.

    The class does not launch Unreal. Callers own the safety of raw commands;
    :meth:`query` is intended for scripts that only assign ``_uepy_result``.
    """

    def __init__(
        self,
        *,
        engine_root: str | os.PathLike[str] | None = None,
        discovery_timeout: float = 2.0,
        project: str | None = None,
        node: str | None = None,
        cwd: str | os.PathLike[str] | None = None,
    ) -> None:
        self.remote_path = find_remote_execution(engine_root)
        self.remote = _load_remote_module(self.remote_path)
        self.discovery_timeout = max(0.1, discovery_timeout)
        self.project = project
        self.node = node
        self.cwd = str(Path(cwd or os.getcwd()).resolve())
        self.session: Any | None = None
        self.connected_node: dict[str, Any] | None = None

    def __enter__(self) -> "UnrealRemoteClient":
        self.start()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()

    def start(self) -> None:
        if self.session is None:
            self.session = self.remote.RemoteExecution()
            self.session.start()

    def close(self) -> None:
        if self.session is not None:
            self.session.stop()
            self.session = None
            self.connected_node = None

    def discover(self) -> list[dict[str, Any]]:
        self.start()
        deadline = time.monotonic() + self.discovery_timeout
        nodes: list[dict[str, Any]] = []
        while time.monotonic() < deadline:
            nodes = list(self.session.remote_nodes)
            time.sleep(0.05)
        return sorted(nodes, key=lambda item: (item.get("project_name", ""), item.get("node_id", "")))

    def select_node(self, nodes: Iterable[dict[str, Any]]) -> dict[str, Any]:
        matches = list(nodes)
        if not matches:
            raise DiscoveryError(
                "No Unreal Editor with Python Remote Execution enabled was discovered. "
                "Confirm the editor is running and the Python multicast settings use loopback."
            )

        if self.node:
            needle = self.node.casefold()
            matches = [
                item
                for item in matches
                if str(item.get("node_id", "")).casefold().startswith(needle)
            ]
            if not matches:
                raise DiscoveryError(f"No discovered node matches --node {self.node!r}.")

        if self.project:
            needle = self.project.casefold()
            matches = [
                item
                for item in matches
                if needle in str(item.get("project_name", "")).casefold()
                or needle in str(item.get("project_root", "")).casefold()
            ]
            if not matches:
                raise DiscoveryError(f"No discovered node matches --project {self.project!r}.")

        if len(matches) > 1 and not self.node and not self.project:
            local_matches = [
                item
                for item in matches
                if _paths_overlap(self.cwd, str(item.get("project_root", "")))
            ]
            if len(local_matches) == 1:
                matches = local_matches

        if len(matches) != 1:
            summary = ", ".join(
                f"{item.get('project_name', '?')}:{str(item.get('node_id', ''))[:8]}"
                for item in matches
            )
            raise DiscoveryError(
                "Multiple Unreal Editor nodes match. Select one with --project or --node: " + summary
            )
        return matches[0]

    def connect(self) -> dict[str, Any]:
        if self.connected_node is not None:
            return self.connected_node
        node = self.select_node(self.discover())
        try:
            self.session.open_command_connection(node["node_id"])
        except Exception as exc:  # Epic's helper exposes RuntimeError only informally.
            raise DiscoveryError(f"Could not connect to Unreal node {node['node_id']}: {exc}") from exc
        self.connected_node = node
        return node

    def run(self, command: str, *, mode: str, raise_on_failure: bool = True) -> dict[str, Any]:
        self.connect()
        try:
            response = self.session.run_command(
                command,
                unattended=True,
                exec_mode=mode,
                raise_on_failure=False,
            )
        except Exception as exc:
            raise ProtocolError(
                "Remote execution failed. Large responses can exceed Epic's single-message "
                f"client buffer; narrow the query if applicable. Details: {exc}"
            ) from exc

        if raise_on_failure and not response.get("success", False):
            details = response.get("result") or "unknown Unreal Python error"
            raise RemoteCommandError(str(details))
        return response

    def evaluate(self, expression: str) -> dict[str, Any]:
        return self.run(expression, mode=self.remote.MODE_EVAL_STATEMENT)

    def execute(self, script: str) -> dict[str, Any]:
        return self.run(script, mode=self.remote.MODE_EXEC_FILE)

    def query(self, body: str) -> Any:
        """Execute an inspection body and decode its ``_uepy_result`` value."""

        marker = f"__UEPY_RESULT_{uuid.uuid4().hex}__"
        wrapped = (
            "import json\n"
            "_uepy_result = None\n"
            f"{body.rstrip()}\n"
            f"print({marker!r} + json.dumps(_uepy_result, ensure_ascii=False, separators=(',', ':')))\n"
        )
        response = self.execute(wrapped)
        for entry in response.get("output", []):
            text = str(entry.get("output", ""))
            position = text.find(marker)
            if position < 0:
                continue
            payload = text[position + len(marker) :].strip()
            try:
                return json.loads(payload)
            except json.JSONDecodeError as exc:
                raise ProtocolError(
                    "Unreal returned a truncated or invalid JSON inspection result. "
                    "Reduce the requested result limit."
                ) from exc
        raise ProtocolError(
            "The Unreal command succeeded but did not return the expected inspection marker."
        )

