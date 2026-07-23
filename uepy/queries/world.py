"""Editor-world query builders."""

from __future__ import annotations

def world() -> str:
    return r'''
def _uepy_path(value):
    if value is None:
        return None
    try:
        return value.get_path_name()
    except Exception:
        return str(value)

def _uepy_vec(value):
    return {"x": value.x, "y": value.y, "z": value.z}

def _uepy_box(value):
    if value is None:
        return None
    return {"min": _uepy_vec(value.min), "max": _uepy_vec(value.max), "is_valid": bool(value.is_valid)}

_uepy_editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
_uepy_actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
_uepy_world = _uepy_editor.get_editor_world()
_uepy_settings = _uepy_world.get_world_settings()
_uepy_package = _uepy_world.get_package()
_uepy_dirty_packages = {
    package.get_path_name()
    for package in (
        list(unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages())
        + list(unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages())
    )
}
_uepy_partition = False
_uepy_actor_desc_count = None
_uepy_editor_bounds = None
_uepy_runtime_bounds = None
try:
    _uepy_descs = unreal.WorldPartitionBlueprintLibrary.get_actor_descs()
    _uepy_partition = _uepy_descs is not None
    _uepy_actor_desc_count = len(_uepy_descs) if _uepy_descs is not None else None
    _uepy_editor_bounds = _uepy_box(unreal.WorldPartitionBlueprintLibrary.get_editor_world_bounds())
    _uepy_runtime_bounds = _uepy_box(unreal.WorldPartitionBlueprintLibrary.get_runtime_world_bounds())
except Exception:
    pass
_uepy_result = {
    "world": _uepy_world.get_path_name(),
    "package": _uepy_package.get_path_name(),
    "package_dirty": _uepy_package.get_path_name() in _uepy_dirty_packages,
    "game_mode_override": _uepy_path(_uepy_settings.get_editor_property("default_game_mode")),
    "world_partition": _uepy_partition,
    "actor_descriptor_count": _uepy_actor_desc_count,
    "loaded_actor_count": len(_uepy_actor_subsystem.get_all_level_actors()),
    "selected_actor_count": len(_uepy_actor_subsystem.get_selected_level_actors()),
    "editor_bounds": _uepy_editor_bounds,
    "runtime_bounds": _uepy_runtime_bounds,
}
'''
