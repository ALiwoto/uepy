"""Loaded-actor and World Partition descriptor query builders."""

from __future__ import annotations

import json

ACTOR_HELPERS = r'''
def _uepy_path(value):
    if value is None:
        return None
    try:
        return value.get_path_name()
    except Exception:
        return str(value)

def _uepy_vec(value):
    return {"x": value.x, "y": value.y, "z": value.z}

def _uepy_rot(value):
    return {"pitch": value.pitch, "yaw": value.yaw, "roll": value.roll}

def _uepy_property(obj, name, default=None):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return default

def _uepy_actor(actor):
    location = actor.get_actor_location()
    rotation = actor.get_actor_rotation()
    scale = actor.get_actor_scale3d()
    try:
        bounds_origin, bounds_extent = actor.get_actor_bounds(False)
        bounds = {"origin": _uepy_vec(bounds_origin), "extent": _uepy_vec(bounds_extent)}
    except Exception:
        bounds = None
    data_layers = []
    for layer in (_uepy_property(actor, "data_layer_assets", []) or []):
        data_layers.append(_uepy_path(layer))
    mesh_path = None
    try:
        component = actor.get_component_by_class(unreal.StaticMeshComponent)
        if component:
            mesh_path = _uepy_path(component.get_editor_property("static_mesh"))
    except Exception:
        pass
    return {
        "label": actor.get_actor_label(),
        "name": actor.get_name(),
        "path": actor.get_path_name(),
        "class": actor.get_class().get_path_name(),
        "folder": str(actor.get_folder_path()),
        "location": _uepy_vec(location),
        "rotation": _uepy_rot(rotation),
        "scale": _uepy_vec(scale),
        "bounds": bounds,
        "is_spatially_loaded": bool(_uepy_property(actor, "is_spatially_loaded", False)),
        "runtime_grid": str(_uepy_property(actor, "runtime_grid", "")),
        "data_layers": data_layers,
        "static_mesh": mesh_path,
    }
'''


def selected(limit: int) -> str:
    return ACTOR_HELPERS + f'''
_uepy_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
_uepy_actors = list(_uepy_subsystem.get_selected_level_actors())
_uepy_result = {{
    "count": len(_uepy_actors),
    "actors": [_uepy_actor(actor) for actor in _uepy_actors[:{limit}]],
}}
'''


def actors(match: str | None, class_match: str | None, limit: int) -> str:
    match_literal = json.dumps((match or "").casefold())
    class_literal = json.dumps((class_match or "").casefold())
    return ACTOR_HELPERS + f'''
_uepy_match = {match_literal}
_uepy_class_match = {class_literal}
_uepy_all = list(unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors())
_uepy_filtered = []
for _uepy_item in _uepy_all:
    _uepy_label = _uepy_item.get_actor_label()
    _uepy_class = _uepy_item.get_class().get_path_name()
    if _uepy_match and _uepy_match not in _uepy_label.casefold() and _uepy_match not in _uepy_item.get_name().casefold():
        continue
    if _uepy_class_match and _uepy_class_match not in _uepy_class.casefold():
        continue
    _uepy_filtered.append(_uepy_item)
_uepy_result = {{
    "loaded_count": len(_uepy_all),
    "matching_count": len(_uepy_filtered),
    "actors": [_uepy_actor(actor) for actor in _uepy_filtered[:{limit}]],
}}
'''


def actor(label: str, limit: int) -> str:
    label_literal = json.dumps(label.casefold())
    return ACTOR_HELPERS + f'''
_uepy_label = {label_literal}
_uepy_all = list(unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors())
_uepy_exact = [actor for actor in _uepy_all if actor.get_actor_label().casefold() == _uepy_label]
_uepy_matches = _uepy_exact or [actor for actor in _uepy_all if _uepy_label in actor.get_actor_label().casefold()]
_uepy_result = {{
    "exact": bool(_uepy_exact),
    "matching_count": len(_uepy_matches),
    "actors": [_uepy_actor(actor) for actor in _uepy_matches[:{limit}]],
}}
'''


def actor_descriptors(match: str | None, limit: int) -> str:
    match_literal = json.dumps((match or "").casefold())
    return r'''
def _uepy_vec(value):
    return {"x": value.x, "y": value.y, "z": value.z}

def _uepy_box(value):
    return {"min": _uepy_vec(value.min), "max": _uepy_vec(value.max), "is_valid": bool(value.is_valid)}

def _uepy_path(value):
    try:
        return value.get_path_name()
    except Exception:
        return str(value)
''' + f'''
_uepy_match = {match_literal}
_uepy_descs = list(unreal.WorldPartitionBlueprintLibrary.get_actor_descs() or [])
_uepy_filtered = []
for _uepy_desc in _uepy_descs:
    if _uepy_match and _uepy_match not in str(_uepy_desc.label).casefold() and _uepy_match not in str(_uepy_desc.name).casefold():
        continue
    _uepy_filtered.append(_uepy_desc)
_uepy_result = {{
    "descriptor_count": len(_uepy_descs),
    "matching_count": len(_uepy_filtered),
    "actors": [{{
        "label": str(item.label),
        "name": str(item.name),
        "actor_path": str(item.actor_path),
        "actor_package": str(item.actor_package),
        "native_class": _uepy_path(item.native_class),
        "bounds": _uepy_box(item.bounds),
        "runtime_grid": str(item.runtime_grid),
        "is_spatially_loaded": bool(item.is_spatially_loaded),
        "is_editor_only": bool(item.actor_is_editor_only),
        "data_layers": [str(layer) for layer in item.data_layer_assets],
    }} for item in _uepy_filtered[:{limit}]],
}}
'''
