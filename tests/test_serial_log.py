import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch, MagicMock, call

spec = importlib.util.spec_from_file_location("serial_log", Path(__file__).resolve().parents[1] / "tools/serial_log.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class OwnershipTest(unittest.TestCase):
    def test_serial_open_follows_idf_no_reset_order(self):
        events = []
        class Port:
            def __setattr__(self, key, value):
                events.append((key, value))
            def open(self):
                events.append(("open", None))
        module.open_without_reset(Port())
        self.assertEqual(events, [("dtr", True), ("rts", True), ("open", None), ("rts", False), ("dtr", False)])

    def test_running_computer_blocks_takeover(self):
        with patch.object(module.socket, "create_connection", return_value=MagicMock()) as connect:
            self.assertTrue(module.computer_running())
        connect.assert_called_once_with(("127.0.0.1", 80), timeout=1)

    def test_desktop_port_is_checked_after_docker_port(self):
        with patch.object(module.socket, "create_connection",
                          side_effect=[ConnectionRefusedError(), MagicMock()]) as connect:
            self.assertTrue(module.computer_running())
        self.assertEqual(connect.call_args_list, [call(("127.0.0.1", 80), timeout=1),
                                                  call(("127.0.0.1", 8089), timeout=1)])

    def test_custom_port_does_not_replace_standard_ports(self):
        with patch.object(module.socket, "create_connection",
                          side_effect=[ConnectionRefusedError(), ConnectionRefusedError(), MagicMock()]) as connect:
            self.assertTrue(module.computer_running(8888))
        self.assertEqual([c.args[0][1] for c in connect.call_args_list], [80, 8089, 8888])

    def test_closed_port_allows_takeover(self):
        with patch.object(module.socket, "create_connection", side_effect=ConnectionRefusedError) as connect:
            self.assertFalse(module.computer_running())
        self.assertEqual(connect.call_count, 2)

    def test_unknown_network_error_fails_closed(self):
        with patch.object(module.socket, "create_connection", side_effect=TimeoutError):
            with self.assertRaises(RuntimeError):
                module.computer_running()

    def test_second_port_error_fails_closed(self):
        with patch.object(module.socket, "create_connection",
                          side_effect=[ConnectionRefusedError(), TimeoutError()]):
            with self.assertRaises(RuntimeError):
                module.computer_running()

    def test_docker_listener_rejects_active_before_opening_usb(self):
        with patch("sys.argv", ["serial_log.py", "--port", "/dev/example", "--command", "mode active"]), \
             patch.object(module.socket, "create_connection", return_value=MagicMock()), \
             patch.object(module.serial, "Serial") as serial_port:
            with self.assertRaises(SystemExit) as stopped:
                module.main()
        self.assertEqual(stopped.exception.code, 2)
        serial_port.assert_not_called()


if __name__ == "__main__":
    unittest.main()
