# uepy

`uepy` is a dependency-free Python module and CLI for inspecting a running
Unreal Editor through Epic's **Python Editor Script Plugin**. It uses the
`remote_execution.py` client shipped with the installed Unreal Engine, so the
protocol stays aligned with the local engine rather than being reimplemented.

The useful distinction from an Unreal commandlet is that `uepy` connects to the
editor that is already open. Queries can therefore see loaded objects, current
selection, and unsaved actor changes.

## Requirements

- Python 3.10 or newer.
- A running Unreal Editor with **Python Editor Script Plugin** enabled.
- **Enable Remote Execution** under Project Settings → Plugins → Python.
- For local use, keep Multicast Bind Address `127.0.0.1` and TTL `0`.

No third-party Python packages are required.

## Installation

Install the checkout once in editable mode:

```powershell
python -m pip install -e .
```

This creates the `uepy` console command while keeping imports pointed at the
checkout. Ordinary `.py` edits take effect immediately. Reinstall only after
changing packaging metadata, dependencies, or console entry points in
`pyproject.toml`, moving the checkout, or switching Python environments.

Then run it from any directory:

```powershell
uepy nodes
uepy world
uepy selected
```

Without installation, `python -m uepy ...` remains available while the current
directory is this repository.

## Inspection commands

```powershell
uepy nodes
uepy status
uepy world
uepy selected --limit 10
uepy actors --match Clenfield
uepy actor PS_Clenfield_Initial
uepy descriptors --match Clenfield
uepy asset /Game/LevelPrototyping/Meshes/SM_Cube
uepy mesh /Game/LevelPrototyping/Meshes/SM_Cube
```

`descriptors` uses World Partition actor descriptors, so it can report actors
that are not currently loaded. Result limits are capped at 25 because Epic's
bundled remote client reads a command response into one small protocol buffer.

Every inspection command emits JSON. Add `--compact` before the command for
machine-oriented single-line output:

```powershell
uepy --compact actor PS_Clenfield_Initial
```

If several editors are open, select one explicitly:

```powershell
uepy --project Peacebound world
uepy --node CC9F5DF0 world
```

The helper is located automatically through `--engine-root`, the
`UEPY_ENGINE_ROOT`/`UE_ENGINE_ROOT` environment variables, Epic's Windows
installation manifest, or standard Epic Games installation folders.

## Raw execution

Raw commands are available for debugging, but deliberately require an explicit
acknowledgement because Unreal does not enforce read-only Python execution:

```powershell
uepy eval --unsafe "unreal.SystemLibrary.get_engine_version()"
uepy exec --unsafe --file inspect_something.py
```

An expression can call mutating functions, despite being called `eval`. Do not
use raw execution against valuable editor state unless the command has been
reviewed. Prefer the built-in inspection commands.

## Python API

```python
from uepy import UnrealRemoteClient
from uepy import queries

with UnrealRemoteClient(project="Peacebound") as client:
    node = client.connect()
    current_world = client.query(queries.world())
```

`UnrealRemoteClient` never launches Unreal. Failure to discover a node is
reported as an error so callers can decide whether launching an editor is
appropriate.

## Scope and limitations

- This is editor tooling, not packaged-game runtime integration.
- Python exposes Unreal-reflected APIs. Some non-reflected C++ internals may
  require a small editor-only C++ bridge.
- Inspection can load a requested asset into editor memory, but the built-in
  commands never set properties, save packages, import content, or delete data.
- Live queries run on the editor's main thread. Keep them small and do not issue
  concurrent commands while the editor is busy.
