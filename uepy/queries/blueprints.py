"""Read-only Blueprint graph query builders."""

from __future__ import annotations

import json

BRIDGE_PROTOCOL_VERSION = 2


def blueprint(path: str, graph: str) -> str:
    path_literal = json.dumps(path)
    graph_literal = json.dumps(graph)
    return f'''\
_uepy_bridge = getattr(unreal, "UEPyBlueprintGraphBridge", None)
if _uepy_bridge is None:
    _uepy_result = {{
        "bridge_available": False,
        "found": False,
        "requested_path": {path_literal},
        "requested_graph": {graph_literal},
        "error": "UEPyEditorBridge is not loaded. Enable the editor-only plugin for this project.",
    }}
else:
    _uepy_bridge_version = _uepy_bridge.get_bridge_protocol_version()
    if _uepy_bridge_version != {BRIDGE_PROTOCOL_VERSION}:
        _uepy_result = {{
            "bridge_available": True,
            "found": False,
            "requested_path": {path_literal},
            "requested_graph": {graph_literal},
            "bridge_protocol_version": _uepy_bridge_version,
            "expected_bridge_protocol_version": {BRIDGE_PROTOCOL_VERSION},
            "error": "The loaded UEPyEditorBridge protocol is incompatible with this uepy client.",
        }}
    else:
        _uepy_blueprint_json = _uepy_bridge.inspect_blueprint_graph_json(
            {path_literal},
            {graph_literal},
        )
        _uepy_result = json.loads(_uepy_blueprint_json)
        _uepy_result["bridge_available"] = True
'''


def blueprint_patch(path: str, graph: str, patch_json: str, *, apply: bool) -> str:
    path_literal = json.dumps(path)
    graph_literal = json.dumps(graph)
    patch_literal = json.dumps(patch_json)
    bridge_method = (
        "apply_blueprint_graph_patch_json"
        if apply
        else "validate_blueprint_graph_patch_json"
    )
    return f'''\
_uepy_bridge = getattr(unreal, "UEPyBlueprintGraphBridge", None)
if _uepy_bridge is None:
    _uepy_result = {{
        "bridge_available": False,
        "valid": False,
        "applied": False,
        "requested_path": {path_literal},
        "requested_graph": {graph_literal},
        "error": "UEPyEditorBridge is not loaded. Enable the editor-only plugin for this project.",
    }}
else:
    _uepy_bridge_version = _uepy_bridge.get_bridge_protocol_version()
    if _uepy_bridge_version != {BRIDGE_PROTOCOL_VERSION}:
        _uepy_result = {{
            "bridge_available": True,
            "valid": False,
            "applied": False,
            "requested_path": {path_literal},
            "requested_graph": {graph_literal},
            "bridge_protocol_version": _uepy_bridge_version,
            "expected_bridge_protocol_version": {BRIDGE_PROTOCOL_VERSION},
            "error": "The loaded UEPyEditorBridge protocol is incompatible with this uepy client.",
        }}
    else:
        _uepy_patch_result_json = _uepy_bridge.{bridge_method}(
            {path_literal},
            {graph_literal},
            {patch_literal},
        )
        _uepy_result = json.loads(_uepy_patch_result_json)
        _uepy_result["bridge_available"] = True
'''
