"""Generic Unreal asset query builders."""

from __future__ import annotations

import json

def asset(path: str) -> str:
    path_literal = json.dumps(path)
    return f'''
_uepy_path = {path_literal}
_uepy_asset = unreal.load_asset(_uepy_path)
if _uepy_asset is None:
    _uepy_result = {{"found": False, "requested_path": _uepy_path}}
else:
    _uepy_package = _uepy_asset.get_package()
    _uepy_dirty_packages = {{
        package.get_path_name()
        for package in (
            list(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages())
            + list(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages())
        )
    }}
    _uepy_result = {{
        "found": True,
        "name": _uepy_asset.get_name(),
        "path": _uepy_asset.get_path_name(),
        "class": _uepy_asset.get_class().get_path_name(),
        "package": _uepy_package.get_path_name(),
        "package_dirty": _uepy_package.get_path_name() in _uepy_dirty_packages,
    }}
'''
