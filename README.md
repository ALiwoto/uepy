# uepy

`uepy` is a dependency-free Python module and CLI for inspecting a running
Unreal Editor and applying explicitly reviewed editor patches through Epic's
**Python Editor Script Plugin**. It uses the
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
uepy actors --match Village
uepy actor BP_VillageGate
uepy descriptors --match Village
uepy asset /Game/LevelPrototyping/Meshes/SM_Cube
uepy blueprint /Game/Characters/Hero/Animations/ABP_Hero --graph AnimGraph
uepy mesh /Game/LevelPrototyping/Meshes/SM_Cube
uepy material /Game/Materials/MI_Example
```

`material` reports effective rendering properties, the complete parent chain,
base-property overrides, scalar/vector/texture/static-switch parameters, used
textures, loaded actor users, and Asset Registry dependencies/referencers. Use
`--parameters overrides` for only parameters overridden on the inspected
instance, or `--parameters none` for a compact structural report. It is a
read-only inspection command; material edits still require explicit
`exec --unsafe` usage.

`blueprint` uses the optional `UEPyEditorBridge` plugin to report a
graph's nodes, stable node and pin IDs, positions, pin types/defaults, every
connection, and concise connection flow. Animation Blueprints also report
useful authored details for sequence players, slots, layered bone blends,
cached poses, and state machines. The command only loads and reads the asset;
it never compiles, dirties, changes, or saves it.

The bridge is distributed with this repository under
`UnrealPlugins/UEPyEditorBridge`. Add its parent directory to the consuming
project's `.uproject`, adjusting the relative path for that checkout:

```json
"AdditionalPluginDirectories": [
    "../Tools/uepy/UnrealPlugins"
]
```

Then enable only the editor target:

```json
{
    "Name": "UEPyEditorBridge",
    "Enabled": true,
    "TargetAllowList": ["Editor"]
}
```

Other inspection commands do not require the plugin. If it is absent or its
protocol version differs from the Python client, `uepy blueprint` returns a
structured error instead of attempting incomplete graph inspection.

### Blueprint graph patches

Inspection includes a graph `fingerprint`. A patch must include that exact
fingerprint so a graph changed after inspection cannot be edited accidentally.
The patch format supports connecting existing pins, disconnecting an existing
link, moving existing nodes, and creating a small reviewed set of Animation
Blueprint nodes:

```json
{
    "version": 1,
    "expected_fingerprint": "COPY_FROM_INSPECTION",
    "operations": [
        {
            "op": "move_node",
            "node_id": "NODE_GUID",
            "x": -640,
            "y": 160
        },
        {
            "op": "connect",
            "from_node_id": "OUTPUT_NODE_GUID",
            "from_pin_id": "OUTPUT_PIN_GUID",
            "to_node_id": "INPUT_NODE_GUID",
            "to_pin_id": "INPUT_PIN_GUID"
        }
    ]
}
```

Node creation uses a separate patch so the new GUIDs and pins can be inspected
before anything is connected. The supported creation operations are
`add_save_cached_pose`, `add_use_cached_pose`, `add_slot`, and
`add_layered_bone_blend`:

```json
{
    "version": 1,
    "expected_fingerprint": "COPY_FROM_INSPECTION",
    "operations": [
        {
            "op": "add_save_cached_pose",
            "alias": "spell_pose_save",
            "cache_name": "SpellPose",
            "x": -900,
            "y": 300
        },
        {
            "op": "add_use_cached_pose",
            "alias": "spell_pose_use",
            "cache_name": "SpellPose",
            "x": -600,
            "y": 300
        },
        {
            "op": "add_slot",
            "alias": "spell_slot",
            "slot_name": "DefaultSlot",
            "always_update_source_pose": false,
            "x": -300,
            "y": 300
        },
        {
            "op": "add_layered_bone_blend",
            "alias": "right_arm_blend",
            "x": 0,
            "y": 300,
            "default_weight": 1.0,
            "mesh_space_rotation_blend": false,
            "mesh_space_scale_blend": false,
            "branch_filters": [
                {
                    "bone": "clavicle_r",
                    "blend_depth": 1
                }
            ]
        }
    ]
}
```

Aliases must be unique within the patch. Cached-pose names must be unique in
the Blueprint, and a use node may reference an existing save node or one added
earlier in the same patch. Slot names must already be registered on the target
skeleton; this operation deliberately does not edit the skeleton. Branch-filter
bones must exist on that skeleton. A successful apply result includes
`created_nodes` with each alias, node GUID, pins, position, and inspected node
details.

Validation is read-only and is the default:

```powershell
uepy blueprint /Game/Characters/Hero/Animations/ABP_Hero --graph AnimGraph --patch change.json
```

Applying requires two explicit acknowledgements:

```powershell
uepy blueprint /Game/Characters/Hero/Animations/ABP_Hero --graph AnimGraph --patch change.json --apply --unsafe
```

Application is one editor Undo transaction and never compiles or saves the asset.
It refuses dirty packages, stale fingerprints, unknown operations, reused pins,
implicit conversion nodes, and connections that would automatically break
other links. Creation operations cannot be mixed with connect, disconnect, or
move operations: create, inspect, then connect with the resulting GUIDs in a
second patch. A single patch is limited to 100 operations.

`descriptors` uses World Partition actor descriptors, so it can report actors
that are not currently loaded. Result limits are capped at 100 to keep live
main-thread inspection bounded. `uepy` patches UE 5.4's bundled TCP receiver at
runtime so command-result JSON split across multiple network chunks is assembled
before decoding; the installed engine files are never modified.

Every inspection command emits JSON. Add `--compact` before the command for
machine-oriented single-line output:

```powershell
uepy --compact actor BP_VillageGate
```

If several editors are open, select one explicitly:

```powershell
uepy --project MyGame world
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

`eval` expressions run in a lambda-local namespace. `exec` runs each script in
a disposable function scope and performs Python garbage collection afterward.
This prevents command-local actors, worlds, and packages from remaining rooted
by Unreal's Python reference collector and causing a fatal world-leak check
during a later map change. Definitions and variables created by one raw command
therefore do not persist into the next command.

## Python API

```python
from uepy import UnrealRemoteClient
from uepy import queries

with UnrealRemoteClient(project="MyGame") as client:
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
