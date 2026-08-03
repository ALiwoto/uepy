import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from uepy import queries
from uepy.cli import _parser, main
from uepy.queries.blueprints import BRIDGE_PROTOCOL_VERSION


class BlueprintQueryTests(unittest.TestCase):
    def test_query_calls_editor_bridge_with_escaped_arguments(self) -> None:
        query = queries.blueprint('/Game/Test/ABP_"Hero"', "AnimGraph")

        self.assertIn('getattr(unreal, "UEPyBlueprintGraphBridge", None)', query)
        self.assertIn("_uepy_bridge.inspect_blueprint_graph_json", query)
        self.assertIn("_uepy_bridge.get_bridge_protocol_version()", query)
        self.assertIn(str(BRIDGE_PROTOCOL_VERSION), query)
        self.assertIn('"/Game/Test/ABP_\\"Hero\\""', query)
        self.assertIn('"AnimGraph"', query)
        self.assertIn("json.loads", query)

    def test_query_reports_missing_optional_bridge(self) -> None:
        query = queries.blueprint("/Game/Test/ABP_Hero", "AnimGraph")

        self.assertIn('"bridge_available": False', query)
        self.assertIn("UEPyEditorBridge is not loaded", query)

        scope = {"json": json, "unreal": SimpleNamespace()}
        exec(query, scope)
        self.assertFalse(scope["_uepy_result"]["bridge_available"])

    def test_query_decodes_compatible_bridge_response(self) -> None:
        class CompatibleBridge:
            @staticmethod
            def get_bridge_protocol_version() -> int:
                return BRIDGE_PROTOCOL_VERSION

            @staticmethod
            def inspect_blueprint_graph_json(path: str, graph: str) -> str:
                return json.dumps(
                    {"found": True, "path": path, "graph": {"name": graph}}
                )

        query = queries.blueprint("/Game/Test/ABP_Hero", "AnimGraph")
        scope = {
            "json": json,
            "unreal": SimpleNamespace(UEPyBlueprintGraphBridge=CompatibleBridge),
        }
        exec(query, scope)

        result = scope["_uepy_result"]
        self.assertTrue(result["bridge_available"])
        self.assertTrue(result["found"])
        self.assertEqual(result["graph"]["name"], "AnimGraph")

    def test_cli_requires_and_parses_graph_name(self) -> None:
        args = _parser().parse_args(
            ["blueprint", "/Game/Test/ABP_Hero", "--graph", "AnimGraph"]
        )

        self.assertEqual(args.command, "blueprint")
        self.assertEqual(args.path, "/Game/Test/ABP_Hero")
        self.assertEqual(args.graph, "AnimGraph")

    def test_cli_requires_unsafe_for_patch_application(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            patch_path = Path(temporary) / "patch.json"
            patch_path.write_text("{}", encoding="utf-8")
            with self.assertRaises(SystemExit):
                main(
                    [
                        "blueprint",
                        "/Game/Test/ABP_Hero",
                        "--graph",
                        "AnimGraph",
                        "--patch",
                        str(patch_path),
                        "--apply",
                    ]
                )

    def test_patch_query_uses_validation_by_default(self) -> None:
        class CompatibleBridge:
            @staticmethod
            def get_bridge_protocol_version() -> int:
                return BRIDGE_PROTOCOL_VERSION

            @staticmethod
            def validate_blueprint_graph_patch_json(
                path: str, graph: str, patch_json: str
            ) -> str:
                return json.dumps(
                    {
                        "valid": True,
                        "applied": False,
                        "path": path,
                        "graph": graph,
                        "patch": json.loads(patch_json),
                    }
                )

        patch = json.dumps(
            {
                "version": 1,
                "expected_fingerprint": "abc",
                "operations": [
                    {
                        "op": "move_node",
                        "node_id": "00000000-0000-0000-0000-000000000001",
                        "x": 10,
                        "y": 20,
                    }
                ],
            }
        )
        query = queries.blueprint_patch(
            "/Game/Test/ABP_Hero", "AnimGraph", patch, apply=False
        )
        scope = {
            "json": json,
            "unreal": SimpleNamespace(UEPyBlueprintGraphBridge=CompatibleBridge),
        }
        exec(query, scope)

        result = scope["_uepy_result"]
        self.assertTrue(result["bridge_available"])
        self.assertTrue(result["valid"])
        self.assertFalse(result["applied"])
        self.assertEqual(result["patch"]["operations"][0]["op"], "move_node")

    def test_patch_query_calls_apply_only_when_requested(self) -> None:
        class CompatibleBridge:
            @staticmethod
            def get_bridge_protocol_version() -> int:
                return BRIDGE_PROTOCOL_VERSION

            @staticmethod
            def apply_blueprint_graph_patch_json(
                path: str, graph: str, patch_json: str
            ) -> str:
                return json.dumps({"valid": True, "applied": True})

        query = queries.blueprint_patch(
            "/Game/Test/ABP_Hero", "AnimGraph", "{}", apply=True
        )
        scope = {
            "json": json,
            "unreal": SimpleNamespace(UEPyBlueprintGraphBridge=CompatibleBridge),
        }
        exec(query, scope)

        self.assertTrue(scope["_uepy_result"]["applied"])


if __name__ == "__main__":
    unittest.main()
