"""Generic Unreal asset query builders."""

from __future__ import annotations

import json


def asset(path: str) -> str:
    path_literal = json.dumps(path)
    return f"""
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
"""


def duplicate_asset(
    source_path: str, destination_path: str, force: bool = False
) -> str:
    source_literal = json.dumps(source_path)
    destination_literal = json.dumps(destination_path)
    force_literal = "True" if force else "False"
    return f"""
_uepy_source_path = {source_literal}
_uepy_destination_path = {destination_literal}
_uepy_force = {force_literal}
_uepy_destination_existed = unreal.EditorAssetLibrary.does_asset_exist(
    _uepy_destination_path
)
_uepy_result = {{
    "duplicated": False,
    "source": _uepy_source_path,
    "destination": _uepy_destination_path,
    "forced": _uepy_force,
    "replaced_existing": False,
}}

if _uepy_source_path == _uepy_destination_path:
    _uepy_result["error"] = "Source and destination must be different assets."
elif not unreal.EditorAssetLibrary.does_asset_exist(_uepy_source_path):
    _uepy_result["error"] = "Source asset does not exist."
elif _uepy_destination_existed and not _uepy_force:
    _uepy_result["error"] = (
        "Destination asset already exists; pass --force to replace it."
    )
elif _uepy_destination_existed and not unreal.EditorAssetLibrary.delete_asset(
    _uepy_destination_path
):
    _uepy_result["error"] = "Could not delete the existing destination asset."
else:
    _uepy_duplicated = unreal.EditorAssetLibrary.duplicate_asset(
        _uepy_source_path,
        _uepy_destination_path,
    )
    if not _uepy_duplicated:
        _uepy_result["error"] = "Unreal could not duplicate the asset."
    elif not unreal.EditorAssetLibrary.save_asset(
        _uepy_destination_path,
        only_if_is_dirty=False,
    ):
        _uepy_result["error"] = "The duplicate was created but could not be saved."
    else:
        _uepy_asset = unreal.load_asset(_uepy_destination_path)
        _uepy_result = {{
            "duplicated": True,
            "source": _uepy_source_path,
            "destination": _uepy_destination_path,
            "path": (
                _uepy_asset.get_path_name()
                if _uepy_asset is not None
                else _uepy_destination_path
            ),
            "class": (
                _uepy_asset.get_class().get_path_name()
                if _uepy_asset is not None
                else None
            ),
            "forced": _uepy_force,
            "replaced_existing": _uepy_destination_existed,
            "saved": True,
        }}
"""
