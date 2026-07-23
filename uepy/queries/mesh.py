"""Static-mesh query builders."""

from __future__ import annotations

import json

def mesh(path: str) -> str:
    path_literal = json.dumps(path)
    return f'''
def _uepy_vec(value):
    return {{"x": value.x, "y": value.y, "z": value.z}}

_uepy_path = {path_literal}
_uepy_mesh = unreal.load_asset(_uepy_path)
if _uepy_mesh is None:
    _uepy_result = {{"found": False, "requested_path": _uepy_path}}
elif not isinstance(_uepy_mesh, unreal.StaticMesh):
    _uepy_result = {{
        "found": True,
        "is_static_mesh": False,
        "path": _uepy_mesh.get_path_name(),
        "class": _uepy_mesh.get_class().get_path_name(),
    }}
else:
    _uepy_box = _uepy_mesh.get_bounding_box()
    _uepy_bounds = _uepy_mesh.get_bounds()
    _uepy_package = _uepy_mesh.get_package()
    _uepy_dirty_packages = {{
        package.get_path_name()
        for package in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()
    }}
    _uepy_materials = []
    for _uepy_index in range(len(_uepy_mesh.get_editor_property("static_materials"))):
        _uepy_material = _uepy_mesh.get_material(_uepy_index)
        _uepy_materials.append(_uepy_material.get_path_name() if _uepy_material else None)
    _uepy_result = {{
        "found": True,
        "is_static_mesh": True,
        "name": _uepy_mesh.get_name(),
        "path": _uepy_mesh.get_path_name(),
        "package_dirty": _uepy_package.get_path_name() in _uepy_dirty_packages,
        "bounding_box": {{
            "min": _uepy_vec(_uepy_box.min),
            "max": _uepy_vec(_uepy_box.max),
            "is_valid": bool(_uepy_box.is_valid),
        }},
        "bounds": {{
            "origin": _uepy_vec(_uepy_bounds.origin),
            "box_extent": _uepy_vec(_uepy_bounds.box_extent),
            "sphere_radius": _uepy_bounds.sphere_radius,
        }},
        "num_lods": _uepy_mesh.get_num_lods(),
        "materials": _uepy_materials,
    }}
'''
