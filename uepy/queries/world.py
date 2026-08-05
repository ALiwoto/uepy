"""Active editor or PIE world query builders."""

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

def _uepy_call_world_getter(name):
    try:
        return getattr(_uepy_editor, name)()
    except Exception:
        return None

_uepy_game_world = _uepy_call_world_getter("get_game_world")
_uepy_editor_world = _uepy_call_world_getter("get_editor_world")
if _uepy_game_world is not None:
    _uepy_world = _uepy_game_world
    _uepy_world_kind = "game"
elif _uepy_editor_world is not None:
    _uepy_world = _uepy_editor_world
    _uepy_world_kind = "editor"
else:
    # Some editor travel paths temporarily make both subsystem getters return
    # None even though the PIE world is alive. ObjectIterator is a generic
    # Unreal fallback; prefer the persistent /Game UEDPIE world over streamed
    # /Temp Level Instance worlds and editor-loaded map assets.
    _uepy_world_candidates = []
    try:
        for _uepy_candidate in unreal.ObjectIterator(unreal.World):
            _uepy_candidate_path = _uepy_candidate.get_path_name()
            try:
                _uepy_candidate.get_world_settings()
            except Exception:
                continue
            if "UEDPIE_" in _uepy_candidate_path and _uepy_candidate_path.startswith("/Game/"):
                _uepy_score = 0
            elif "UEDPIE_" in _uepy_candidate_path:
                _uepy_score = 1
            elif _uepy_candidate_path.startswith("/Game/"):
                _uepy_score = 2
            else:
                continue
            _uepy_world_candidates.append(
                (_uepy_score, _uepy_candidate_path, _uepy_candidate)
            )
    except Exception:
        pass
    _uepy_world_candidates.sort(key=lambda item: (item[0], item[1]))
    if not _uepy_world_candidates:
        raise RuntimeError("No active Unreal editor or PIE world was found.")
    _uepy_world = _uepy_world_candidates[0][2]
    _uepy_world_kind = (
        "game-fallback"
        if "UEDPIE_" in _uepy_world_candidates[0][1]
        else "editor-fallback"
    )

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
if _uepy_world_kind.startswith("editor"):
    try:
        _uepy_descs = unreal.WorldPartitionBlueprintLibrary.get_actor_descs()
        _uepy_partition = _uepy_descs is not None
        _uepy_actor_desc_count = len(_uepy_descs) if _uepy_descs is not None else None
        _uepy_editor_bounds = _uepy_box(unreal.WorldPartitionBlueprintLibrary.get_editor_world_bounds())
        _uepy_runtime_bounds = _uepy_box(unreal.WorldPartitionBlueprintLibrary.get_runtime_world_bounds())
    except Exception:
        pass

if _uepy_world_kind.startswith("game"):
    try:
        _uepy_loaded_actor_count = len(
            unreal.GameplayStatics.get_all_actors_of_class(_uepy_world, unreal.Actor)
        )
    except Exception:
        _uepy_loaded_actor_count = None
    _uepy_selected_actor_count = None
else:
    _uepy_loaded_actor_count = len(_uepy_actor_subsystem.get_all_level_actors())
    _uepy_selected_actor_count = len(_uepy_actor_subsystem.get_selected_level_actors())

_uepy_result = {
    "world": _uepy_world.get_path_name(),
    "world_kind": _uepy_world_kind,
    "package": _uepy_package.get_path_name(),
    "package_dirty": _uepy_package.get_path_name() in _uepy_dirty_packages,
    "game_mode_override": _uepy_path(_uepy_settings.get_editor_property("default_game_mode")),
    "world_partition": _uepy_partition,
    "actor_descriptor_count": _uepy_actor_desc_count,
    "loaded_actor_count": _uepy_loaded_actor_count,
    "selected_actor_count": _uepy_selected_actor_count,
    "editor_bounds": _uepy_editor_bounds,
    "runtime_bounds": _uepy_runtime_bounds,
}
'''
