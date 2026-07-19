import unittest

from uepy.client import _paths_overlap


class PathOverlapTests(unittest.TestCase):
    def test_parent_and_child_overlap(self) -> None:
        self.assertTrue(_paths_overlap("C:/work/project", "C:/work/project/Game"))

    def test_unrelated_paths_do_not_overlap(self) -> None:
        self.assertFalse(_paths_overlap("C:/work/one", "C:/work/two"))


if __name__ == "__main__":
    unittest.main()

