import multiprocessing
import time
import unittest

from uepy.locking import EditorCommandLock


def _hold_lock(node_id, acquired, release):
    with EditorCommandLock(node_id):
        acquired.set()
        release.wait(10.0)


def _wait_for_lock(node_id, attempting, elapsed_queue):
    attempting.set()
    started = time.monotonic()
    with EditorCommandLock(node_id):
        elapsed_queue.put(time.monotonic() - started)


class EditorCommandLockTests(unittest.TestCase):
    def test_processes_targeting_same_node_wait_their_turn(self) -> None:
        context = multiprocessing.get_context("spawn")
        acquired = context.Event()
        release = context.Event()
        attempting = context.Event()
        elapsed_queue = context.Queue()

        holder = context.Process(target=_hold_lock, args=("shared-node", acquired, release))
        waiter = context.Process(
            target=_wait_for_lock,
            args=("shared-node", attempting, elapsed_queue),
        )
        holder.start()
        self.assertTrue(acquired.wait(5.0))
        waiter.start()
        self.assertTrue(attempting.wait(5.0))

        time.sleep(0.2)
        self.assertTrue(waiter.is_alive())
        release.set()

        holder.join(5.0)
        waiter.join(5.0)
        self.assertEqual(holder.exitcode, 0)
        self.assertEqual(waiter.exitcode, 0)
        self.assertGreaterEqual(elapsed_queue.get(timeout=1.0), 0.15)


if __name__ == "__main__":
    unittest.main()
