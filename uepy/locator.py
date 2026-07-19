"""Locate Epic's version-matched ``remote_execution.py`` helper."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Iterable

from .errors import UepyError


REMOTE_RELATIVE_TO_ENGINE = Path(
    "Plugins/Experimental/PythonScriptPlugin/Content/Python/remote_execution.py"
)


def _expand_candidate(value: str | os.PathLike[str]) -> list[Path]:
    path = Path(value).expanduser()
    candidates = [path]
    if path.name.lower() == "remote_execution.py":
        return candidates
    candidates.append(path / REMOTE_RELATIVE_TO_ENGINE)
    candidates.append(path / "Engine" / REMOTE_RELATIVE_TO_ENGINE)
    return candidates


def _launcher_installations() -> Iterable[Path]:
    if os.name != "nt":
        return []

    program_data = Path(os.environ.get("PROGRAMDATA", r"C:\ProgramData"))
    manifest = program_data / "Epic/UnrealEngineLauncher/LauncherInstalled.dat"
    if not manifest.is_file():
        return []

    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []

    results: list[Path] = []
    for installation in data.get("InstallationList", []):
        location = installation.get("InstallLocation")
        if location:
            results.append(Path(location))
    return results


def _standard_installations() -> Iterable[Path]:
    results: list[Path] = []
    if os.name == "nt":
        roots = {
            Path(os.environ.get("ProgramFiles", r"C:\Program Files")),
            Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")),
        }
        for root in roots:
            results.extend((root / "Epic Games").glob("UE_*"))
    else:
        results.extend(Path("/Users/Shared/Epic Games").glob("UE_*"))
        results.extend(Path("/opt").glob("UnrealEngine*"))
    return results


def candidate_remote_execution_files(
    engine_root: str | os.PathLike[str] | None = None,
) -> list[Path]:
    """Return plausible helpers, in priority order, without requiring existence."""

    sources: list[str | os.PathLike[str]] = []
    if engine_root:
        sources.append(engine_root)
    for name in ("UEPY_REMOTE_EXECUTION", "UEPY_ENGINE_ROOT", "UE_ENGINE_ROOT"):
        value = os.environ.get(name)
        if value:
            sources.append(value)
    sources.extend(_launcher_installations())
    sources.extend(sorted(_standard_installations(), reverse=True))

    candidates: list[Path] = []
    seen: set[str] = set()
    for source in sources:
        for candidate in _expand_candidate(source):
            key = os.path.normcase(os.path.abspath(candidate))
            if key not in seen:
                seen.add(key)
                candidates.append(candidate)
    return candidates


def find_remote_execution(
    engine_root: str | os.PathLike[str] | None = None,
) -> Path:
    """Find Epic's remote client or raise an actionable error."""

    candidates = candidate_remote_execution_files(engine_root)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()

    checked = "\n".join(f"  - {path}" for path in candidates[:12])
    if not checked:
        checked = "  (no Unreal installations were discovered)"
    raise UepyError(
        "Could not find PythonScriptPlugin's remote_execution.py. "
        "Pass --engine-root or set UEPY_ENGINE_ROOT. Checked:\n" + checked
    )

