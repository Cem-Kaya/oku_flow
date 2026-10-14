"""Contract tests use local socket fixtures, never a real adb server or camera."""
from contextlib import nullcontext
from unittest.mock import patch
import importlib.util
import json
from pathlib import Path
import socket
import struct
import tempfile
import threading
import time
import unittest

spec = importlib.util.spec_from_file_location("probe", Path(__file__).resolve().parents[2] / "scripts" / "android_camera_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class FragmentedStream:
    def __init__(self, data):
        self.data = data
    def recv(self, size):
        result, self.data = self.data[:min(1, size)], self.data[min(1, size):]
        return result


class FakeAdb:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.requests = []
        self.commands = []
        self.peers = []
        self.token = "fixture-token"
        self.control_ready = threading.Event()
        self.start_requested = threading.Event()
        self.control = None
        self.stopped = threading.Event()
        self.worker = threading.Thread(target=self.accept, daemon=True)
        self.worker.start()

    def accept(self):
        while not self.stopped.is_set():
            try:
                stream, _ = self.listener.accept()
                self.peers.append(stream)
                threading.Thread(target=self.peer, args=(stream,), daemon=True).start()
            except OSError:
                return

    def request(self, stream):
        size = int(probe.read_exact(stream, 4), 16)
        value = probe.read_exact(stream, size).decode()
        self.requests.append(value)
        stream.sendall(b"OKAY")
        return value

    def send(self, stream, kind, value):
        payload = probe.json_bytes(value)
        stream.sendall(probe.HEADER.pack(kind, 0, len(payload)) + payload)

    def peer(self, stream):
        try:
            request = self.request(stream)
            if request == "host:track-devices-proto-binary":
                # Windows USB transports can have no bus-address/usb: field.
                usb = b"\x0a\x09usb-phone\x10\x08\x2a\x07Fixture\x38\x01\x50\x01"
                wifi = b"\x0a\x04wifi\x10\x08\x38\x02\x50\x02"
                body = bytes([10, len(usb)]) + usb + bytes([10, len(wifi)]) + wifi
                stream.sendall(f"{len(body):04x}".encode() + body)
                return
            if request == "host:devices-l":
                body = b"usb-phone device usb:1-2 model:Fixture transport_id:1\n192.168.1.2:5555 device model:Wifi transport_id:2\n"
                stream.sendall(f"{len(body):04x}".encode() + body)
                return
            if request != "host:transport-id:1":
                raise AssertionError(request)
            request = self.request(stream)
            if request != "localabstract:okuflow_camera":
                raise AssertionError(request)
            magic, version, role, size = struct.unpack("<4sHHI", probe.read_exact(stream, 12))
            if (magic, version) != (b"OKUF", 1):
                raise AssertionError("Wrong hello")
            hello = json.loads(probe.read_exact(stream, size))
            if role == 1 and hello.get("probe"):
                self.send(stream, probe.ACK, {"protocol": 1, "probe": True, "busy": False, "problem": None, "cameras": []})
                return
            if role == 1:
                self.control = stream
                self.send(stream, probe.ACK, {"protocol": 1, "token": self.token, "cameras": [], "codecs": ["avc"]})
                self.control_ready.set()
                while not self.stopped.is_set():
                    kind, flags, payload = probe.read_packet(stream)
                    if (kind, flags) != (probe.COMMAND, 0):
                        raise AssertionError("Wrong command framing")
                    command = json.loads(payload)
                    self.commands.append(command)
                    if command["op"] == "start":
                        self.send(stream, probe.STATE, {"event": "configured", "generation": 1,
                            "width": 640, "height": 480, "fps": 30, "bitrate": 4_000_000, "mime": "video/avc"})
                        self.start_requested.set()
            else:
                if hello["token"] != self.token:
                    raise AssertionError("Wrong video binding")
                self.send(stream, probe.ACK, {"channel": "video"})
                if not self.start_requested.wait(2):
                    raise AssertionError("No start")
                sequence = 0
                while not self.stopped.is_set():
                    payload = probe.FRAME_PREFIX.pack(sequence * 33333, sequence, 1) + b"\0\0\0\1\x65\x01"
                    stream.sendall(probe.HEADER.pack(probe.FRAME, 1, len(payload)) + payload)
                    sequence += 1
                    time.sleep(0.015)
        except (OSError, EOFError):
            pass
        finally:
            stream.close()

    def close(self):
        self.stopped.set()
        self.listener.close()
        for stream in self.peers:
            try:
                stream.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            stream.close()


class HostProbeTests(unittest.TestCase):
    def test_launch_prefers_play_package(self):
        adb = probe.AdbServer()
        with patch.object(adb, 'transport', return_value=nullcontext(FragmentedStream(b'Starting: Intent'))) as transport:
            adb.launch('fixture')
        transport.assert_called_once_with('fixture', 'shell:am start -n com.okuflow/com.okuflow.camera.MainActivity')

    def test_launch_legacy_only_when_play_activity_missing(self):
        adb = probe.AdbServer()
        replies = [nullcontext(FragmentedStream(b'Error type 3\nActivity class does not exist.')),
                   nullcontext(FragmentedStream(b'Starting: Intent'))]
        with patch.object(adb, 'transport', side_effect=replies) as transport:
            adb.launch('fixture')
        self.assertEqual(transport.call_count, 2)
        self.assertEqual(transport.call_args.args, ('fixture', 'shell:am start -n com.okuflow.camera/.MainActivity'))

    def test_blocked_play_activity_does_not_launch_legacy(self):
        adb = probe.AdbServer()
        with patch.object(adb, 'transport', return_value=nullcontext(FragmentedStream(b'Error: Activity start blocked'))) as transport:
            with self.assertRaisesRegex(RuntimeError, 'blocked'):
                adb.launch('fixture')
        self.assertEqual(transport.call_count, 1)

    def test_probe_does_not_claim_session(self):
        server = FakeAdb()
        try:
            client = probe.PhoneCameraClient(probe.AdbServer(server.port), "usb-phone")
            reply = client.probe()
            self.assertTrue(reply["probe"])
            self.assertFalse(reply["busy"])
            self.assertNotIn("token", reply)
            self.assertIsNone(server.control)
            self.assertEqual(server.commands, [])
        finally:
            server.close()

    def test_golden_packet_and_fragmentation(self):
        self.assertEqual(probe.read_packet(FragmentedStream(bytes([2, 0, 0, 0, 2, 0, 0, 0, 123, 125]))), (2, 0, b"{}"))

    def test_rejects_oversized_and_truncated(self):
        with self.assertRaises(ValueError):
            probe.read_packet(FragmentedStream(probe.HEADER.pack(2, 0, 65537)))
        with self.assertRaises(EOFError):
            probe.read_packet(FragmentedStream(probe.HEADER.pack(2, 0, 2) + b"{"))

    def test_usb_evidence_and_authorization_are_separate(self):
        devices = probe.usb_devices("a unauthorized usb:1 model:A\nb device usb:2\n192.168.1.1:5555 device\nemulator-5554 device usb:3\nc device model:C\n")
        self.assertEqual([(device["serial"], device["state"]) for device in devices], [("a", "unauthorized"), ("b", "device")])

    def test_fake_adb_two_channels_capture_and_remote_controls(self):
        server = FakeAdb()
        client = probe.PhoneCameraClient(probe.AdbServer(server.port), "usb-phone")
        try:
            self.assertEqual(client.adb.devices()[0]["serial"], "usb-phone")
            client.connect()
            client.command("zoom", ratio=2)
            client.command("camera", id="1")
            with tempfile.TemporaryDirectory() as root:
                output = Path(root) / "capture"
                # Validate the protocol, not scheduler/TCP delivery within 50 ms.
                # Keep sending until the capture closes its own fixture socket.
                result = probe.capture(client, output, 1.0)
                self.assertGreaterEqual(result["generations"][1]["frames"], 3)
                self.assertTrue((output / "camera-g1.h264").is_file())
                self.assertTrue((output / "summary.json").is_file())
            self.assertEqual([command["op"] for command in server.commands[:3]], ["zoom", "camera", "start"])
            self.assertEqual(server.commands[1]["cameraId"], "1")
            self.assertEqual(server.commands[1]["id"], 2)
            self.assertFalse(any("forward" in request or "kill" in request for request in server.requests))
        finally:
            client.close()
            server.close()

    def test_json_rejects_non_finite_controls(self):
        with self.assertRaises(ValueError):
            probe.json_bytes({"op": "zoom", "ratio": float("nan")})

    def test_native_windows_usb_without_bus_path(self):
        usb = b"\x0a\x03abc\x10\x08\x38\x01\x50\x05"
        wifi = b"\x0a\x03xyz\x10\x08\x38\x02"
        data = bytes([10, len(usb)]) + usb + bytes([10, len(wifi)]) + wifi
        self.assertEqual(probe.protobuf_usb_devices(data), [{"serial": "abc", "state": "device", "connection_type": "usb", "transport_id": 5}])

    def test_protobuf_rejects_invalid_lengths_and_varints(self):
        for data in (b"\x0a\xff", b"\x0a\x0a\x01", b"\x08" + b"\xff" * 10):
            with self.assertRaises(ValueError):
                probe.protobuf_usb_devices(data)


if __name__ == "__main__":
    unittest.main()
