"""Generic Unreal asset query builders."""

from __future__ import annotations

import json


def extract_embedded_audio(
    source_path: str,
    destination_directory: str,
    force: bool = False,
) -> str:
    source_literal = json.dumps(source_path)
    destination_literal = json.dumps(destination_directory)
    force_literal = "True" if force else "False"
    return f"""
_uepy_source_path = {source_literal}
_uepy_destination_directory = {destination_literal}
_uepy_force = {force_literal}
_uepy_result = {{
    "extracted": False,
    "source": _uepy_source_path,
    "destination": _uepy_destination_directory,
    "forced": _uepy_force,
}}
_uepy_bridge = getattr(unreal, "UEPyAudioAssetBridge", None)
_uepy_result_enum = getattr(unreal, "UEPyEmbeddedAudioExtractionResult", None)
if _uepy_bridge is None or _uepy_result_enum is None:
    _uepy_result["error"] = (
        "UEPyEditorBridge 0.4.0 or newer is required for embedded-audio extraction."
    )
else:
    (
        _uepy_extraction_result,
        _uepy_scanned_packages,
        _uepy_extracted_waves,
        _uepy_skipped_packages,
        _uepy_failed_packages,
        _uepy_written_files,
        _uepy_errors,
    ) = _uepy_bridge.extract_embedded_wave_audio(
        _uepy_source_path,
        _uepy_destination_directory,
        _uepy_force,
    )
    _uepy_result.update({{
        "extracted": _uepy_extraction_result == _uepy_result_enum.SUCCESS,
        "result": str(_uepy_extraction_result),
        "scanned_packages": _uepy_scanned_packages,
        "extracted_waves": _uepy_extracted_waves,
        "skipped_packages": _uepy_skipped_packages,
        "failed_packages": _uepy_failed_packages,
        "written_files": list(_uepy_written_files),
        "errors": list(_uepy_errors),
    }})
    if _uepy_errors:
        _uepy_result["error"] = _uepy_errors[0]
"""


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


def bake_shadow_proxy(
    source_path: str,
    destination_path: str | None = None,
    triangle_fraction: float = 0.01,
    force: bool = False,
) -> str:
    source_literal = json.dumps(source_path)
    destination_literal = json.dumps(destination_path or "")
    fraction_literal = repr(triangle_fraction)
    force_literal = "True" if force else "False"
    return f"""
_uepy_source_path = {source_literal}
_uepy_requested_destination_path = {destination_literal}
_uepy_triangle_fraction = {fraction_literal}
_uepy_force = {force_literal}
_uepy_result = {{
    "baked": False,
    "source": _uepy_source_path,
    "requested_destination": _uepy_requested_destination_path or None,
    "triangle_fraction": _uepy_triangle_fraction,
    "forced": _uepy_force,
}}
_uepy_bridge = getattr(unreal, "UEPyStaticMeshAssetBridge", None)
_uepy_result_enum = getattr(unreal, "UEPyShadowProxyBakeResult", None)
if _uepy_bridge is None or _uepy_result_enum is None:
    _uepy_result["error"] = (
        "UEPyEditorBridge 0.3.0 or newer is required for shadow-proxy baking."
    )
else:
    (
        _uepy_bake_result,
        _uepy_destination_path,
        _uepy_source_triangles,
        _uepy_proxy_triangles,
        _uepy_saved_package_bytes,
        _uepy_error,
    ) = _uepy_bridge.bake_shadow_proxy(
        _uepy_source_path,
        _uepy_requested_destination_path,
        _uepy_triangle_fraction,
        _uepy_force,
    )
    _uepy_result.update({{
        "baked": _uepy_bake_result == _uepy_result_enum.SUCCESS,
        "result": str(_uepy_bake_result),
        "destination": _uepy_destination_path,
        "source_triangles": _uepy_source_triangles,
        "proxy_triangles": _uepy_proxy_triangles,
        "saved_package_bytes": _uepy_saved_package_bytes,
    }})
    if _uepy_error:
        _uepy_result["error"] = _uepy_error
"""
