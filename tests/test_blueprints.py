import json
import unittest
from types import SimpleNamespace

from uepy import queries
from uepy.cli import _parser
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


if __name__ == "__main__":
    unittest.main()
