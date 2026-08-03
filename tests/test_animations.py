import json
import unittest
from types import SimpleNamespace

from uepy import queries
from uepy.cli import _parser, main
from uepy.queries.animations import BRIDGE_PROTOCOL_VERSION


class AnimationQueryTests(unittest.TestCase):
    def test_inspection_calls_compatible_bridge(self) -> None:
        class CompatibleBridge:
            @staticmethod
            def get_bridge_protocol_version() -> int:
                return BRIDGE_PROTOCOL_VERSION

            @staticmethod
            def inspect_animation_sequence_json(path: str) -> str:
                return json.dumps({"found": True, "path": path, "number_of_keys": 2})

        query = queries.animation('/Game/Test/A_"Pose"')
        scope = {
            "json": json,
            "unreal": SimpleNamespace(UEPyAnimationSequenceBridge=CompatibleBridge),
        }
        exec(query, scope)

        result = scope["_uepy_result"]
        self.assertTrue(result["bridge_available"])
        self.assertTrue(result["found"])
        self.assertEqual(result["number_of_keys"], 2)
        self.assertEqual(result["path"], '/Game/Test/A_"Pose"')

    def test_promotion_validation_uses_frame_and_fingerprint(self) -> None:
        class CompatibleBridge:
            @staticmethod
            def get_bridge_protocol_version() -> int:
                return BRIDGE_PROTOCOL_VERSION

            @staticmethod
            def validate_promote_frame_to_start_json(
                path: str, frame_index: int, fingerprint: str
            ) -> str:
                return json.dumps(
                    {
                        "valid": True,
                        "applied": False,
                        "path": path,
                        "frame_index": frame_index,
                        "expected_fingerprint": fingerprint,
                    }
                )

        query = queries.promote_animation_frame(
            "/Game/Test/A_Pose", 1, "abc123", apply=False
        )
        scope = {
            "json": json,
            "unreal": SimpleNamespace(UEPyAnimationSequenceBridge=CompatibleBridge),
        }
        exec(query, scope)

        result = scope["_uepy_result"]
        self.assertTrue(result["valid"])
        self.assertFalse(result["applied"])
        self.assertEqual(result["frame_index"], 1)
        self.assertEqual(result["expected_fingerprint"], "abc123")

    def test_cli_requires_fingerprint_for_promotion(self) -> None:
        with self.assertRaises(SystemExit):
            main(["animation", "/Game/Test/A_Pose", "--promote-frame", "1"])

    def test_cli_requires_unsafe_for_apply(self) -> None:
        with self.assertRaises(SystemExit):
            main(
                [
                    "animation",
                    "/Game/Test/A_Pose",
                    "--promote-frame",
                    "1",
                    "--expected-fingerprint",
                    "abc123",
                    "--apply",
                ]
            )


if __name__ == "__main__":
    unittest.main()
