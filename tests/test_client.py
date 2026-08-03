import unittest
from unittest.mock import patch

import uepy.client as client_module
from uepy.client import (
    UnrealRemoteClient,
    _paths_overlap,
    _receive_complete_json_bytes,
    _scoped_eval_expression,
    _scoped_exec_script,
)
from uepy import queries


class FakeSocket:
    def __init__(self, chunks: list[bytes]) -> None:
        self.chunks = list(chunks)
        self.timeout: float | None = None

    def recv(self, size: int) -> bytes:
        del size
        return self.chunks.pop(0) if self.chunks else b""

    def gettimeout(self) -> float | None:
        return self.timeout

    def settimeout(self, timeout: float | None) -> None:
        self.timeout = timeout


class PathOverlapTests(unittest.TestCase):
    def test_parent_and_child_overlap(self) -> None:
        self.assertTrue(_paths_overlap("C:/work/project", "C:/work/project/Game"))

    def test_unrelated_paths_do_not_overlap(self) -> None:
        self.assertFalse(_paths_overlap("C:/work/one", "C:/work/two"))


class ChunkedReceiveTests(unittest.TestCase):
    def test_combines_tcp_chunks_until_json_is_complete(self) -> None:
        chunks = [b'{"success":true,"data":"', b"a" * 9000, b'"}']
        expected = b"".join(chunks)
        command_socket = FakeSocket(chunks)

        response = _receive_complete_json_bytes(command_socket)

        self.assertEqual(response, expected)
        self.assertIsNone(command_socket.timeout)

    def test_rejects_a_connection_that_closes_mid_response(self) -> None:
        command_socket = FakeSocket([b'{"success":true', b""])

        with self.assertRaisesRegex(RuntimeError, "closed before"):
            _receive_complete_json_bytes(command_socket)


class ScopedExecutionTests(unittest.TestCase):
    def test_wraps_eval_expression_in_lambda_scope(self) -> None:
        wrapped = _scoped_eval_expression("(world := unreal.EditorLevelLibrary.get_editor_world())")

        self.assertEqual(
            wrapped,
            "(lambda: ((world := unreal.EditorLevelLibrary.get_editor_world())))()",
        )
        compile(wrapped, "<uepy-test>", "eval")

    def test_wraps_script_in_disposable_scope(self) -> None:
        wrapped = _scoped_exec_script("world = unreal.EditorLevelLibrary.get_editor_world()", "_scope")

        self.assertIn("def _scope():", wrapped)
        self.assertIn("    world = unreal.EditorLevelLibrary.get_editor_world()", wrapped)
        self.assertIn("finally:", wrapped)
        self.assertIn("    del _scope", wrapped)
        self.assertIn("_uepy_gc.collect()", wrapped)
        compile(wrapped, "<uepy-test>", "exec")

    def test_empty_script_is_valid(self) -> None:
        wrapped = _scoped_exec_script("", "_scope")

        self.assertIn("    pass", wrapped)
        compile(wrapped, "<uepy-test>", "exec")


class CommandLockLifecycleTests(unittest.TestCase):
    def _client(self, events: list[str], *, fail_open: bool = False) -> UnrealRemoteClient:
        class FakeSession:
            def open_command_connection(self, node_id: str) -> None:
                events.append(f"open:{node_id}")
                if fail_open:
                    raise RuntimeError("busy")

            def stop(self) -> None:
                events.append("stop")

        client = UnrealRemoteClient.__new__(UnrealRemoteClient)
        client.session = FakeSession()
        client.connected_node = None
        client.command_lock = None
        client.discover = lambda: [{"node_id": "NODE-1"}]
        client.select_node = lambda nodes: list(nodes)[0]
        return client

    def test_holds_lock_from_before_connect_until_close(self) -> None:
        events: list[str] = []

        class FakeLock:
            def __init__(self, node_id: str) -> None:
                events.append(f"lock:{node_id}")

            def acquire(self) -> None:
                events.append("acquire")

            def release(self) -> None:
                events.append("release")

        client = self._client(events)
        with patch.object(client_module, "EditorCommandLock", FakeLock):
            client.connect()
            client.close()

        self.assertEqual(
            events,
            ["lock:NODE-1", "acquire", "open:NODE-1", "stop", "release"],
        )

    def test_releases_lock_when_connection_fails(self) -> None:
        events: list[str] = []

        class FakeLock:
            def __init__(self, node_id: str) -> None:
                events.append(f"lock:{node_id}")

            def acquire(self) -> None:
                events.append("acquire")

            def release(self) -> None:
                events.append("release")

        client = self._client(events, fail_open=True)
        with patch.object(client_module, "EditorCommandLock", FakeLock):
            with self.assertRaisesRegex(Exception, "Could not connect"):
                client.connect()

        self.assertEqual(
            events,
            ["lock:NODE-1", "acquire", "open:NODE-1", "release"],
        )

class MaterialQueryTests(unittest.TestCase):
    def test_material_query_escapes_path_and_sets_modes(self) -> None:
        body = queries.material(
            '/Game/Materials/MI_"Quoted"',
            parameter_mode="overrides",
            reference_limit=17,
        )

        self.assertIn('MI_\\"Quoted\\"', body)
        self.assertIn('_uepy_parameter_mode = "overrides"', body)
        self.assertIn("_uepy_reference_limit = 17", body)

    def test_material_query_rejects_unknown_parameter_mode(self) -> None:
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            queries.material("/Game/Materials/M_Test", parameter_mode="everything")


if __name__ == "__main__":
    unittest.main()
