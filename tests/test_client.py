import unittest

from uepy.client import _paths_overlap, _receive_complete_json_bytes


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


if __name__ == "__main__":
    unittest.main()
