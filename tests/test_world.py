from __future__ import annotations

import unittest
from types import SimpleNamespace

from uepy.queries.world import world


class WorldQueryTests(unittest.TestCase):
    def test_prefers_game_world_and_has_live_world_fallback(self) -> None:
        query = world()

        self.assertIn('"get_game_world"', query)
        self.assertIn('"get_editor_world"', query)
        self.assertIn("unreal.ObjectIterator(unreal.World)", query)
        self.assertIn('"world_kind": _uepy_world_kind', query)

    def test_query_remains_project_independent(self) -> None:
        self.assertNotIn("Peacebound", world())

    def test_fallback_selects_persistent_pie_world(self) -> None:
        class FakePackage:
            def __init__(self, path: str) -> None:
                self.path = path

            def get_path_name(self) -> str:
                return self.path

        class FakeSettings:
            def get_editor_property(self, name: str) -> None:
                self.last_property = name
                return None

        class FakeWorld:
            def __init__(self, path: str) -> None:
                self.path = path

            def get_path_name(self) -> str:
                return self.path

            def get_world_settings(self) -> FakeSettings:
                return FakeSettings()

            def get_package(self) -> FakePackage:
                return FakePackage(self.path.split(".", 1)[0])

        class FakeEditor:
            @staticmethod
            def get_game_world() -> None:
                return None

            @staticmethod
            def get_editor_world() -> None:
                return None

        class FakeActorSubsystem:
            @staticmethod
            def get_all_level_actors() -> list[object]:
                return []

            @staticmethod
            def get_selected_level_actors() -> list[object]:
                return []

        editor_type = object()
        actor_subsystem_type = object()
        temp_world = FakeWorld(
            "/Temp/Game/Example/UEDPIE_0_LI_Room_LevelInstance_1.LI_Room"
        )
        persistent_world = FakeWorld(
            "/Game/Maps/UEDPIE_0_L_Gameplay.L_Gameplay"
        )

        fake_unreal = SimpleNamespace(
            UnrealEditorSubsystem=editor_type,
            EditorActorSubsystem=actor_subsystem_type,
            World=object(),
            Actor=object(),
            get_editor_subsystem=lambda subsystem: (
                FakeEditor()
                if subsystem is editor_type
                else FakeActorSubsystem()
            ),
            ObjectIterator=lambda object_type: [temp_world, persistent_world],
            EditorLoadingAndSavingUtils=SimpleNamespace(
                get_dirty_map_packages=lambda: [],
                get_dirty_content_packages=lambda: [],
            ),
            GameplayStatics=SimpleNamespace(
                get_all_actors_of_class=lambda selected_world, actor_type: [
                    object(),
                    object(),
                ]
            ),
            WorldPartitionBlueprintLibrary=SimpleNamespace(),
        )
        scope = {"unreal": fake_unreal}

        exec(world(), scope)

        result = scope["_uepy_result"]
        self.assertEqual(result["world"], persistent_world.get_path_name())
        self.assertEqual(result["world_kind"], "game-fallback")
        self.assertEqual(result["loaded_actor_count"], 2)


if __name__ == "__main__":
    unittest.main()
