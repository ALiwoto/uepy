"""Animation Sequence inspection and reviewed editor-operation query builders."""

from __future__ import annotations

import json

BRIDGE_PROTOCOL_VERSION = 1


def animation(path: str) -> str:
    path_literal = json.dumps(path)
    return f'''\
_uepy_bridge = getattr(unreal, "UEPyAnimationSequenceBridge", None)
if _uepy_bridge is None:
    _uepy_result = {{
        "bridge_available": False,
        "found": False,
        "requested_path": {path_literal},
        "error": "UEPyEditorBridge is not loaded. Enable the editor-only plugin for this project.",
    }}
else:
    _uepy_bridge_version = _uepy_bridge.get_bridge_protocol_version()
    if _uepy_bridge_version != {BRIDGE_PROTOCOL_VERSION}:
        _uepy_result = {{
            "bridge_available": True,
            "found": False,
            "requested_path": {path_literal},
            "bridge_protocol_version": _uepy_bridge_version,
            "expected_bridge_protocol_version": {BRIDGE_PROTOCOL_VERSION},
            "error": "The loaded animation bridge protocol is incompatible with this uepy client.",
        }}
    else:
        _uepy_animation_json = _uepy_bridge.inspect_animation_sequence_json(
            {path_literal}
        )
        _uepy_result = json.loads(_uepy_animation_json)
        _uepy_result["bridge_available"] = True
'''


def promote_animation_frame(
    path: str,
    frame_index: int,
    expected_fingerprint: str,
    *,
    apply: bool,
) -> str:
    path_literal = json.dumps(path)
    fingerprint_literal = json.dumps(expected_fingerprint)
    bridge_method = (
        "apply_promote_frame_to_start_json"
        if apply
        else "validate_promote_frame_to_start_json"
    )
    return f'''\
_uepy_bridge = getattr(unreal, "UEPyAnimationSequenceBridge", None)
if _uepy_bridge is None:
    _uepy_result = {{
        "bridge_available": False,
        "valid": False,
        "applied": False,
        "requested_path": {path_literal},
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
            "bridge_protocol_version": _uepy_bridge_version,
            "expected_bridge_protocol_version": {BRIDGE_PROTOCOL_VERSION},
            "error": "The loaded animation bridge protocol is incompatible with this uepy client.",
        }}
    else:
        _uepy_animation_result_json = _uepy_bridge.{bridge_method}(
            {path_literal},
            {frame_index},
            {fingerprint_literal},
        )
        _uepy_result = json.loads(_uepy_animation_result_json)
        _uepy_result["bridge_available"] = True
'''
