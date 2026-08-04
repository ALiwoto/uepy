import contextlib
import io
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from uepy import queries
from uepy.cli import main


class FakeClass:
    def get_path_name(self) -> str:
        return "/Script/Niagara.NiagaraSystem"


class FakeAsset:
    def __init__(self, path: str) -> None:
        self.path = path

    def get_path_name(self) -> str:
        return self.path + ".N_Destination"

    def get_class(self) -> FakeClass:
        return FakeClass()


class DuplicateAssetQueryTests(unittest.TestCase):
    def _execute(
        self,
        *,
        destination_exists: bool,
        force: bool,
    ) -> tuple[dict, list[tuple]]:
        calls: list[tuple] = []
        destination = "/Game/Test/N_Destination"

        class EditorAssetLibrary:
            @staticmethod
            def does_asset_exist(path: str) -> bool:
                return path == "/Game/Test/N_Source" or (
                    path == destination and destination_exists
                )

            @staticmethod
            def delete_asset(path: str) -> bool:
                calls.append(("delete", path))
                return True

            @staticmethod
            def duplicate_asset(source: str, target: str) -> bool:
                calls.append(("duplicate", source, target))
                return True

            @staticmethod
            def save_asset(path: str, *, only_if_is_dirty: bool) -> bool:
                calls.append(("save", path, only_if_is_dirty))
                return True

        unreal = SimpleNamespace(
            EditorAssetLibrary=EditorAssetLibrary,
            load_asset=lambda path: FakeAsset(path),
        )
        scope = {"unreal": unreal}
        exec(
            queries.duplicate_asset(
                "/Game/Test/N_Source",
                destination,
                force=force,
            ),
            scope,
        )
        return scope["_uepy_result"], calls

    def test_refuses_existing_destination_without_force(self) -> None:
        result, calls = self._execute(destination_exists=True, force=False)

        self.assertFalse(result["duplicated"])
        self.assertIn("--force", result["error"])
        self.assertEqual(calls, [])

    def test_force_replaces_then_duplicates_and_saves(self) -> None:
        result, calls = self._execute(destination_exists=True, force=True)

        self.assertTrue(result["duplicated"])
        self.assertTrue(result["replaced_existing"])
        self.assertEqual(
            calls,
            [
                ("delete", "/Game/Test/N_Destination"),
                (
                    "duplicate",
                    "/Game/Test/N_Source",
                    "/Game/Test/N_Destination",
                ),
                ("save", "/Game/Test/N_Destination", False),
            ],
        )

    def test_escapes_paths_in_generated_query(self) -> None:
        body = queries.duplicate_asset(
            '/Game/Test/N_"Source"',
            '/Game/Test/N_"Destination"',
        )

        self.assertIn('N_\\"Source\\"', body)
        self.assertIn('N_\\"Destination\\"', body)


class DuplicateAssetCliTests(unittest.TestCase):
    def test_existing_destination_returns_nonzero(self) -> None:
        class FakeClient:
            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc, traceback) -> None:
                return None

            def query(self, body: str) -> dict:
                self.body = body
                return {
                    "duplicated": False,
                    "error": "Destination asset already exists; pass --force to replace it.",
                }

        stderr = io.StringIO()
        with patch("uepy.cli._client", return_value=FakeClient()):
            with contextlib.redirect_stderr(stderr):
                exit_code = main(
                    [
                        "duplicate",
                        "/Game/Test/N_Source",
                        "/Game/Test/N_Destination",
                    ]
                )

        self.assertEqual(exit_code, 1)
        self.assertIn("already exists", stderr.getvalue())


class ShadowProxyQueryTests(unittest.TestCase):
    def test_builds_success_report(self) -> None:
        success = object()

        class AssetTools:
            @staticmethod
            def bake_shadow_proxy(
                source: str,
                destination: str,
                fraction: float,
                force: bool,
            ):
                self.assertEqual(source, "/Game/Test/SM_Wall")
                self.assertEqual(destination, "")
                self.assertEqual(fraction, 0.01)
                self.assertTrue(force)
                return (
                    success,
                    "/Game/Test/SM_Wall_Shadow.SM_Wall_Shadow",
                    10_000,
                    100,
                    4_096,
                    "",
                )

        unreal = SimpleNamespace(
            UEPyStaticMeshAssetBridge=AssetTools,
            UEPyShadowProxyBakeResult=SimpleNamespace(SUCCESS=success),
        )
        scope = {"unreal": unreal}
        exec(
            queries.bake_shadow_proxy(
                "/Game/Test/SM_Wall",
                destination_path=None,
                triangle_fraction=0.01,
                force=True,
            ),
            scope,
        )

        result = scope["_uepy_result"]
        self.assertTrue(result["baked"])
        self.assertEqual(result["proxy_triangles"], 100)
        self.assertEqual(result["saved_package_bytes"], 4_096)

    def test_reports_missing_bridge(self) -> None:
        scope = {"unreal": SimpleNamespace()}
        exec(queries.bake_shadow_proxy("/Game/Test/SM_Wall"), scope)

        result = scope["_uepy_result"]
        self.assertFalse(result["baked"])
        self.assertIn("0.3.0", result["error"])


class ShadowProxyCliTests(unittest.TestCase):
    def test_failed_bake_returns_nonzero(self) -> None:
        class FakeClient:
            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc, traceback) -> None:
                return None

            def query(self, body: str) -> dict:
                self.body = body
                return {
                    "baked": False,
                    "error": "Destination already exists; pass --force.",
                }

        stderr = io.StringIO()
        with patch("uepy.cli._client", return_value=FakeClient()):
            with contextlib.redirect_stderr(stderr):
                exit_code = main(["shadow-proxy", "/Game/Test/SM_Wall"])

        self.assertEqual(exit_code, 1)
        self.assertIn("--force", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
