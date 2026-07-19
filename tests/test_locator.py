import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from uepy.locator import REMOTE_RELATIVE_TO_ENGINE, find_remote_execution


class LocatorTests(unittest.TestCase):
    def test_explicit_engine_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            engine = Path(temporary) / "Engine"
            helper = engine / REMOTE_RELATIVE_TO_ENGINE
            helper.parent.mkdir(parents=True)
            helper.write_text("# test", encoding="utf-8")
            with patch.dict(os.environ, {}, clear=True):
                self.assertEqual(find_remote_execution(engine), helper.resolve())

    def test_explicit_install_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            install = Path(temporary) / "UE_5.4"
            helper = install / "Engine" / REMOTE_RELATIVE_TO_ENGINE
            helper.parent.mkdir(parents=True)
            helper.write_text("# test", encoding="utf-8")
            with patch.dict(os.environ, {}, clear=True):
                self.assertEqual(find_remote_execution(install), helper.resolve())


if __name__ == "__main__":
    unittest.main()

