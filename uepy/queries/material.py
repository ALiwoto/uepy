"""Material and material-instance query builders."""

from __future__ import annotations

import json

def material(
    path: str,
    *,
    parameter_mode: str = "all",
    reference_limit: int = 100,
) -> str:
    """Inspect a material interface without modifying it."""

    if parameter_mode not in {"all", "overrides", "none"}:
        raise ValueError(f"Unsupported material parameter mode: {parameter_mode}")
    path_literal = json.dumps(path)
    mode_literal = json.dumps(parameter_mode)
    return r'''
def _uepy_path(value):
    if value is None:
        return None
    try:
        return value.get_path_name()
    except Exception:
        return str(value)

def _uepy_property(obj, name, default=None):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return default

def _uepy_value(value):
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if hasattr(value, "get_path_name"):
        return value.get_path_name()
    if all(hasattr(value, component) for component in ("r", "g", "b", "a")):
        return {"r": value.r, "g": value.g, "b": value.b, "a": value.a}
    try:
        return value.name
    except Exception:
        return str(value)

def _uepy_instance_overrides(instance):
    overrides = _uepy_property(instance, "base_property_overrides")
    if overrides is None:
        return None
    field_pairs = (
        ("override_opacity_mask_clip_value", "opacity_mask_clip_value"),
        ("override_blend_mode", "blend_mode"),
        ("override_shading_model", "shading_model"),
        ("override_dithered_lod_transition", "dithered_lod_transition"),
        ("override_cast_dynamic_shadow_as_masked", "cast_dynamic_shadow_as_masked"),
        ("override_two_sided", "two_sided"),
        ("override_is_thin_surface", "is_thin_surface"),
        ("override_output_translucent_velocity", "output_translucent_velocity"),
        ("override_has_pixel_animation", "has_pixel_animation"),
        ("override_enable_tessellation", "enable_tessellation"),
        ("override_displacement_scaling", "displacement_scaling"),
        ("override_max_world_position_offset_displacement", "max_world_position_offset_displacement"),
    )
    result = {}
    for flag_name, value_name in field_pairs:
        flag = _uepy_property(overrides, flag_name)
        if flag is None:
            continue
        result[flag_name] = bool(flag)
        result[value_name] = _uepy_value(_uepy_property(overrides, value_name))
    return result

def _uepy_local_parameter_names(instance, property_name):
    names = set()
    for entry in (_uepy_property(instance, property_name, []) or []):
        info = _uepy_property(entry, "parameter_info")
        name = _uepy_property(info, "name") if info is not None else None
        if name is not None:
            names.add(str(name))
    return names
''' + f'''
_uepy_requested_path = {path_literal}
_uepy_parameter_mode = {mode_literal}
_uepy_reference_limit = {int(reference_limit)}
''' + r'''
_uepy_asset = unreal.load_asset(_uepy_requested_path)
if _uepy_asset is None:
    _uepy_result = {"found": False, "requested_path": _uepy_requested_path}
elif not isinstance(_uepy_asset, unreal.MaterialInterface):
    _uepy_result = {
        "found": True,
        "is_material_interface": False,
        "name": _uepy_asset.get_name(),
        "path": _uepy_asset.get_path_name(),
        "class": _uepy_asset.get_class().get_path_name(),
    }
else:
    _uepy_package = _uepy_asset.get_package()
    _uepy_dirty_packages = {
        package.get_path_name()
        for package in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
    }

    _uepy_chain_objects = []
    _uepy_current = _uepy_asset
    _uepy_seen = set()
    while _uepy_current is not None and _uepy_current.get_path_name() not in _uepy_seen:
        _uepy_seen.add(_uepy_current.get_path_name())
        _uepy_chain_objects.append(_uepy_current)
        if isinstance(_uepy_current, unreal.MaterialInstance):
            _uepy_current = _uepy_property(_uepy_current, "parent")
        else:
            break

    _uepy_root = _uepy_chain_objects[-1]
    _uepy_root_property_names = (
        "material_domain",
        "blend_mode",
        "shading_model",
        "two_sided",
        "opacity_mask_clip_value",
        "dithered_lod_transition",
        "cast_dynamic_shadow_as_masked",
        "is_thin_surface",
        "output_translucent_velocity",
        "has_pixel_animation",
        "enable_tessellation",
        "max_world_position_offset_displacement",
        "wireframe",
        "use_material_attributes",
        "tangent_space_normal",
        "used_with_nanite",
    )
    _uepy_effective = {}
    for _uepy_name in _uepy_root_property_names:
        _uepy_raw = _uepy_property(_uepy_root, _uepy_name)
        if _uepy_raw is not None:
            _uepy_effective[_uepy_name] = _uepy_value(_uepy_raw)

    _uepy_override_mapping = (
        ("override_opacity_mask_clip_value", "opacity_mask_clip_value"),
        ("override_blend_mode", "blend_mode"),
        ("override_shading_model", "shading_model"),
        ("override_dithered_lod_transition", "dithered_lod_transition"),
        ("override_cast_dynamic_shadow_as_masked", "cast_dynamic_shadow_as_masked"),
        ("override_two_sided", "two_sided"),
        ("override_is_thin_surface", "is_thin_surface"),
        ("override_output_translucent_velocity", "output_translucent_velocity"),
        ("override_has_pixel_animation", "has_pixel_animation"),
        ("override_enable_tessellation", "enable_tessellation"),
        ("override_max_world_position_offset_displacement", "max_world_position_offset_displacement"),
    )
    for _uepy_instance in reversed(_uepy_chain_objects[:-1]):
        _uepy_overrides = _uepy_instance_overrides(_uepy_instance) or {}
        for _uepy_flag, _uepy_value_name in _uepy_override_mapping:
            if _uepy_overrides.get(_uepy_flag):
                _uepy_effective[_uepy_value_name] = _uepy_overrides.get(_uepy_value_name)

    _uepy_inheritance = []
    for _uepy_item in _uepy_chain_objects:
        _uepy_entry = {
            "name": _uepy_item.get_name(),
            "path": _uepy_item.get_path_name(),
            "class": _uepy_item.get_class().get_path_name(),
        }
        if isinstance(_uepy_item, unreal.MaterialInstance):
            _uepy_entry["base_property_overrides"] = _uepy_instance_overrides(_uepy_item)
        _uepy_inheritance.append(_uepy_entry)

    _uepy_local_overrides = {
        "scalar": _uepy_local_parameter_names(_uepy_asset, "scalar_parameter_values"),
        "vector": _uepy_local_parameter_names(_uepy_asset, "vector_parameter_values"),
        "texture": _uepy_local_parameter_names(_uepy_asset, "texture_parameter_values"),
    } if isinstance(_uepy_asset, unreal.MaterialInstance) else {
        "scalar": set(), "vector": set(), "texture": set()
    }

    _uepy_parameters = None
    _uepy_texture_paths = set()
    if _uepy_parameter_mode != "none":
        _uepy_library = unreal.MaterialEditingLibrary
        _uepy_parameters = {"scalar": [], "vector": [], "texture": [], "static_switch": []}
        _uepy_parameter_specs = (
            (
                "scalar",
                _uepy_library.get_scalar_parameter_names,
                _uepy_library.get_material_instance_scalar_parameter_value,
                _uepy_library.get_material_default_scalar_parameter_value,
            ),
            (
                "vector",
                _uepy_library.get_vector_parameter_names,
                _uepy_library.get_material_instance_vector_parameter_value,
                _uepy_library.get_material_default_vector_parameter_value,
            ),
            (
                "texture",
                _uepy_library.get_texture_parameter_names,
                _uepy_library.get_material_instance_texture_parameter_value,
                _uepy_library.get_material_default_texture_parameter_value,
            ),
            (
                "static_switch",
                _uepy_library.get_static_switch_parameter_names,
                _uepy_library.get_material_instance_static_switch_parameter_value,
                _uepy_library.get_material_default_static_switch_parameter_value,
            ),
        )
        for _uepy_kind, _uepy_names_function, _uepy_instance_value, _uepy_default_value in _uepy_parameter_specs:
            for _uepy_parameter_name in _uepy_names_function(_uepy_asset):
                _uepy_name_text = str(_uepy_parameter_name)
                _uepy_is_local = _uepy_name_text in _uepy_local_overrides.get(_uepy_kind, set())
                if _uepy_parameter_mode == "overrides" and not _uepy_is_local:
                    continue
                try:
                    if isinstance(_uepy_asset, unreal.MaterialInstanceConstant):
                        _uepy_parameter_value = _uepy_instance_value(_uepy_asset, _uepy_parameter_name)
                    else:
                        _uepy_parameter_value = _uepy_default_value(_uepy_root, _uepy_parameter_name)
                except Exception as _uepy_error:
                    _uepy_parameters[_uepy_kind].append({
                        "name": _uepy_name_text,
                        "overridden_here": _uepy_is_local,
                        "error": str(_uepy_error),
                    })
                    continue
                _uepy_serialized_value = _uepy_value(_uepy_parameter_value)
                _uepy_parameters[_uepy_kind].append({
                    "name": _uepy_name_text,
                    "value": _uepy_serialized_value,
                    "overridden_here": _uepy_is_local,
                })
                if _uepy_kind == "texture" and isinstance(_uepy_serialized_value, str):
                    _uepy_texture_paths.add(_uepy_serialized_value)

    if isinstance(_uepy_root, unreal.Material):
        try:
            for _uepy_texture in unreal.MaterialEditingLibrary.get_used_textures(_uepy_root):
                if _uepy_texture is not None:
                    _uepy_texture_paths.add(_uepy_texture.get_path_name())
        except Exception:
            pass

    _uepy_loaded_users = []
    for _uepy_actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        for _uepy_component in _uepy_actor.get_components_by_class(unreal.MeshComponent):
            for _uepy_slot, _uepy_material in enumerate(_uepy_component.get_materials()):
                if _uepy_material is _uepy_asset:
                    _uepy_loaded_users.append({
                        "actor": _uepy_actor.get_actor_label(),
                        "component": _uepy_component.get_path_name(),
                        "slot": _uepy_slot,
                    })

    _uepy_registry = unreal.AssetRegistryHelpers.get_asset_registry()
    _uepy_dependency_options = unreal.AssetRegistryDependencyOptions()
    _uepy_package_name = _uepy_package.get_path_name()
    _uepy_dependencies = sorted(
        str(item)
        for item in _uepy_registry.get_dependencies(_uepy_package_name, _uepy_dependency_options)
    )
    _uepy_referencers = sorted(
        str(item)
        for item in _uepy_registry.get_referencers(_uepy_package_name, _uepy_dependency_options)
    )

    _uepy_result = {
        "found": True,
        "is_material_interface": True,
        "name": _uepy_asset.get_name(),
        "path": _uepy_asset.get_path_name(),
        "class": _uepy_asset.get_class().get_path_name(),
        "package": _uepy_package_name,
        "package_dirty": _uepy_package_name in _uepy_dirty_packages,
        "effective_properties": _uepy_effective,
        "inheritance": _uepy_inheritance,
        "parameters_mode": _uepy_parameter_mode,
        "parameters": _uepy_parameters,
        "used_textures": sorted(_uepy_texture_paths),
        "loaded_actor_users": _uepy_loaded_users,
        "asset_dependencies_count": len(_uepy_dependencies),
        "asset_dependencies": _uepy_dependencies[:_uepy_reference_limit],
        "asset_referencers_count": len(_uepy_referencers),
        "asset_referencers": _uepy_referencers[:_uepy_reference_limit],
    }
'''
