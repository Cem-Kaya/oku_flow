#!/usr/bin/env python3
"""USB-only OkuFlow Camera receiver and protocol probe. Python standard library only."""
from __future__ import annotations

import argparse
import datetime
import json
import math
import os
from pathlib import Path
import socket
import struct
import sys
import threading
import time

VERSION = 1
ACK, COMMAND, STATE, METADATA, ERROR, FRAME = range(1, 7)
MAX_CONTROL = 64 * 1024
MAX_VIDEO = 16 * 1024 * 1024
HEADER = struct.Struct("<HHI")
FRAME_PREFIX = struct.Struct("<qqI")


def read_exact(stream: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = stream.recv(size - len(data))
        if not chunk:
            raise EOFError("Connection closed inside a packet")
        data.extend(chunk)
    return bytes(data)


def read_packet(stream: socket.socket, limit: int = MAX_CONTROL) -> tuple[int, int, bytes]:
    kind, flags, size = HEADER.unpack(read_exact(stream, HEADER.size))
    if size > limit:
        raise ValueError(f"Packet length {size} exceeds {limit}")
    return kind, flags, read_exact(stream, size)


def json_bytes(value: dict) -> bytes:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode("utf-8")


def send_hello(stream: socket.socket, role: int, value: dict) -> None:
    body = json_bytes(value)
    if not 2 <= len(body) <= MAX_CONTROL:
        raise ValueError("Invalid hello size")
    stream.sendall(struct.pack("<4sHHI", b"OKUF", VERSION, role, len(body)) + body)


def adb_request(stream: socket.socket, service: str) -> None:
    request = service.encode("utf-8")
    if len(request) > 0xFFFF:
        raise ValueError("adb service request is too large")
    stream.sendall(f"{len(request):04x}".encode("ascii") + request)
    status = read_exact(stream, 4)
    if status == b"FAIL":
        size = int(read_exact(stream, 4), 16)
        raise RuntimeError(read_exact(stream, size).decode("utf-8", errors="replace"))
    if status != b"OKAY":
        raise ValueError("Invalid adb server status")


def usb_devices(text: str) -> list[dict]:
    """Positive USB evidence is mandatory; Wi-Fi, emulators and unknown transports are excluded."""
    result = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        attributes = dict(field.split(":", 1) for field in fields[2:] if ":" in field)
        if "usb" in attributes and not fields[0].startswith("emulator-"):
            result.append({"serial": fields[0], "state": fields[1], **attributes})
    return result


def protobuf_fields(data: bytes):
    """Bounded reader for adb's public devices.proto wire fields; no protobuf runtime dependency."""
    position = 0
    def varint():
        nonlocal position
        result = 0
        for shift in range(0, 70, 7):
            if position >= len(data):
                raise ValueError("Truncated adb protobuf varint")
            byte = data[position]
            position += 1
            if shift == 63 and byte > 1:
                raise ValueError("Overflowing adb protobuf varint")
            result |= (byte & 127) << shift
            if byte < 128:
                return result
        raise ValueError("Invalid adb protobuf varint")
    while position < len(data):
        tag = varint()
        field, wire = tag >> 3, tag & 7
        if not field:
            raise ValueError("Invalid adb protobuf field")
        if wire == 0:
            value = varint()
        elif wire in (1, 2, 5):
            size = varint() if wire == 2 else (8 if wire == 1 else 4)
            if size > len(data) - position:
                raise ValueError("Truncated adb protobuf field")
            value = data[position:position + size]
            position += size
        else:
            raise ValueError("Unsupported adb protobuf wire type")
        yield field, wire, value


def protobuf_usb_devices(data: bytes) -> list[dict]:
    devices = []
    states = {3: "unauthorized", 4: "no permissions", 6: "offline", 8: "device"}
    for field, wire, value in protobuf_fields(data):
        if (field, wire) != (1, 2):
            continue
        attributes = {number: (kind, value) for number, kind, value in protobuf_fields(value)}
        if attributes.get(7) != (0, 1):  # ConnectionType.USB; bus_address is absent on native Windows adb.
            continue
        if attributes.get(1, (None, None))[0] != 2 or len(attributes[1][1]) > 512:
            raise ValueError("Invalid USB serial")
        serial = attributes[1][1].decode("utf-8", errors="strict")
        if not serial or serial.startswith("emulator-"):
            continue
        device = {"serial": serial, "state": states.get(attributes.get(2, (0, 0))[1], "unknown"), "connection_type": "usb"}
        for number, name in ((5, "model"), (10, "transport_id")):
            if number in attributes:
                kind, content = attributes[number]
                if (number == 5 and kind != 2) or (number == 10 and kind != 0):
                    raise ValueError("Invalid adb device attribute")
                device[name] = content.decode("utf-8", errors="strict") if kind == 2 else content
        devices.append(device)
        if len(devices) > 128:
            raise ValueError("Too many USB transports")
    return devices


class AdbServer:
    """Uses the running loopback server directly. Never starts, upgrades, kills or forwards adb."""
    def __init__(self, port: int = 5037):
        self.port = port
        self.usb_transports = {}

    def connect(self) -> socket.socket:
        return socket.create_connection(("127.0.0.1", self.port), timeout=3)

    def devices(self) -> list[dict]:
        try:
            with self.connect() as stream:
                adb_request(stream, "host:track-devices-proto-binary")
                size = int(read_exact(stream, 4), 16)
                devices = protobuf_usb_devices(read_exact(stream, size))
        except RuntimeError:
            devices = self.legacy_devices()  # Older servers: only explicit usb: evidence is accepted.
        self.usb_transports = {device["serial"]: device.get("transport_id", 0) for device in devices if device["state"] == "device"}
        return devices

    def legacy_devices(self) -> list[dict]:
        with self.connect() as stream:
            adb_request(stream, "host:devices-l")
            size = int(read_exact(stream, 4), 16)
            if size > 1024 * 1024:
                raise ValueError("Oversized adb device listing")
            return usb_devices(read_exact(stream, size).decode("utf-8", errors="strict"))

    def transport(self, serial: str, service: str) -> socket.socket:
        stream = self.connect()
        try:
            # The caller obtained this exact serial from the positively identified USB listing.
            if serial not in self.usb_transports:
                raise RuntimeError("USB transport must be identified and authorized before opening it")
            transport_id = self.usb_transports[serial]
            adb_request(stream, f"host:transport-id:{transport_id}" if transport_id else f"host:transport:{serial}")
            adb_request(stream, service)
            return stream
        except BaseException:
            stream.close()
            raise

    def launch(self, serial: str) -> None:
        with self.transport(serial, "shell:am start -n com.okuflow.camera/.MainActivity") as stream:
            data = bytearray()
            while len(data) < MAX_CONTROL:
                chunk = stream.recv(min(4096, MAX_CONTROL - len(data)))
                if not chunk:
                    break
                data.extend(chunk)
            response = data.decode("utf-8", errors="replace")
            if "Error" in response or "Exception" in response:
                raise RuntimeError(response.strip())


class PhoneCameraClient:
    """One independent control/video pair. All writes share a lock; reads have one owner per socket."""
    def __init__(self, adb: AdbServer, serial: str):
        self.adb, self.serial = adb, serial
        self.control = None
        self.video = None
        self.send_lock = threading.Lock()
        self.command_id = 0
        self.capabilities = {}

    def probe(self) -> dict:
        """Read capabilities/readiness without claiming a new companion's session."""
        if not any(device["serial"] == self.serial and device["state"] == "device" for device in self.adb.devices()):
            raise RuntimeError("The selected phone is not connected and authorized over USB")
        with self.adb.transport(self.serial, "localabstract:okuflow_camera") as stream:
            send_hello(stream, 1, {"probe": True, "codecs": ["avc", "hevc"]})
            kind, flags, payload = read_packet(stream)
            reply = json.loads(payload)
            if (kind, flags) == (ERROR, 0):
                raise RuntimeError(f"{reply.get('code', 'failed')}: {reply.get('error', 'Phone probe failed')}")
            if (kind, flags) != (ACK, 0) or reply.get("protocol") != VERSION:
                raise ValueError("Invalid phone probe acknowledgement")
            if not reply.get("probe") and not reply.get("token"):
                raise ValueError("Unsupported phone probe")
            # An old app may claim a session; immediately closing frees it.
            return {key: value for key, value in reply.items() if key != "token"}

    def connect(self) -> dict:
        try:
            if not any(device["serial"] == self.serial and device["state"] == "device" for device in self.adb.devices()):
                raise RuntimeError("The selected phone is not connected and authorized over USB")
            self.control = self.adb.transport(self.serial, "localabstract:okuflow_camera")
            send_hello(self.control, 1, {"codecs": ["avc", "hevc"]})
            kind, flags, payload = read_packet(self.control)
            if (kind, flags) != (ACK, 0):
                raise ValueError("Missing control acknowledgement")
            self.capabilities = json.loads(payload)
            if self.capabilities.get("protocol") != VERSION:
                raise ValueError("Unsupported phone protocol")
            self.video = self.adb.transport(self.serial, "localabstract:okuflow_camera")
            send_hello(self.video, 2, {"token": self.capabilities["token"]})
            kind, flags, payload = read_packet(self.video)
            if (kind, flags) != (ACK, 0) or json.loads(payload).get("channel") != "video":
                raise ValueError("Missing video acknowledgement")
            self.control.settimeout(15)
            self.video.settimeout(15)
            return self.capabilities
        except BaseException:
            self.close()
            raise

    def command(self, op: str, **values) -> None:
        # Preserve the original prototype's camera-id syntax while keeping
        # correlation IDs numeric and independent of the selected lens.
        if op == "camera" and "id" in values:
            values.setdefault("cameraId", values.pop("id"))
        with self.send_lock:
            self.command_id += 1
            payload = json_bytes({"op": op, "id": self.command_id, **values})
            if len(payload) > MAX_CONTROL:
                raise ValueError("Oversized command")
            self.control.sendall(HEADER.pack(COMMAND, 0, len(payload)) + payload)

    def close(self) -> None:
        for stream in (self.control, self.video):
            if stream is not None:
                try:
                    stream.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                stream.close()


def capture(client: PhoneCameraClient, directory: Path, seconds: float, interactive: bool = False) -> dict:
    directory.mkdir(parents=True, exist_ok=False)
    stopped = threading.Event()
    modes = {}
    condition = threading.Condition()
    failures = []
    metadata_file = (directory / "metadata.jsonl").open("x", encoding="utf-8")

    def controls():
        try:
            while not stopped.is_set():
                kind, flags, payload = read_packet(client.control)
                if flags or kind not in (STATE, METADATA, ERROR):
                    raise ValueError("Unexpected control message")
                value = json.loads(payload)
                metadata_file.write(json.dumps({"type": kind, "arrivalMonotonicNs": time.monotonic_ns(), "data": value}) + "\n")
                if kind == ERROR:
                    raise RuntimeError(value.get("error", "Phone error"))
                if kind == STATE and value.get("event") in ("configured", "streaming"):
                    with condition:
                        modes[value["generation"]] = value
                        condition.notify_all()
                    if value["event"] == "streaming":
                        print(f"Streaming {value['width']}x{value['height']} @ {value['fps']} FPS, {value['mime']}", flush=True)
        except Exception as exception:
            if not stopped.is_set():
                failures.append(exception)
                stopped.set()
                client.close()
                with condition:
                    condition.notify_all()

    def heartbeat():
        while not stopped.wait(5):
            try:
                client.command("ping")
            except Exception:
                return

    def commands():
        print('Enter JSON commands, e.g. {"op":"zoom","ratio":2} or {"op":"camera","cameraId":"1"}.', flush=True)
        for line in sys.stdin:
            if stopped.is_set():
                return
            try:
                command = json.loads(line)
                op = command.pop("op")
                if op != "camera":
                    command.pop("id", None)
                client.command(op, **command)
            except (ValueError, KeyError, TypeError) as exception:
                print(f"Command rejected: {exception}", file=sys.stderr)

    reader = threading.Thread(target=controls, daemon=True, name="phone-control")
    reader.start()
    threading.Thread(target=heartbeat, daemon=True, name="phone-heartbeat").start()
    if interactive:
        threading.Thread(target=commands, daemon=True, name="phone-commands").start()
    files = {}
    sequences = {}
    pts = {}
    stats = {}
    start = time.monotonic()
    try:
        client.command("start")
        while time.monotonic() - start < seconds and not stopped.is_set():
            kind, flags, payload = read_packet(client.video, MAX_VIDEO)
            if kind != FRAME or flags & ~3 or len(payload) <= FRAME_PREFIX.size:
                raise ValueError("Invalid video frame")
            timestamp, sequence, generation = FRAME_PREFIX.unpack_from(payload)
            if sequence != sequences.get(generation, -1) + 1:
                raise ValueError("Missing or reordered encoded packet")
            sequences[generation] = sequence
            if generation not in files:
                with condition:
                    condition.wait_for(lambda: generation in modes or stopped.is_set(), timeout=5)
                if generation not in modes:
                    raise RuntimeError("Missing negotiated stream format")
                extension = "hevc" if modes[generation]["mime"] == "video/hevc" else "h264"
                files[generation] = (directory / f"camera-g{generation}.{extension}").open("xb")
                stats[generation] = {"frames": 0, "bytes": 0, "firstArrival": None, "lastArrival": None, **modes[generation]}
            data = payload[FRAME_PREFIX.size:]
            if not (data.startswith(b"\0\0\0\1") or data.startswith(b"\0\0\1")):
                raise ValueError("Expected Annex B video")
            files[generation].write(data)
            stats[generation]["bytes"] += len(data)
            if not flags & 2:
                if timestamp < 0 or timestamp <= pts.get(generation, -1):
                    raise ValueError("Non-monotonic encoder timestamp")
                pts[generation] = timestamp
                arrival = time.monotonic()
                current = stats[generation]
                current["frames"] += 1
                if current["firstArrival"] is None:
                    current["firstArrival"] = arrival
                current["lastArrival"] = arrival
        if failures:
            raise failures[0]
        if not any(stat["frames"] for stat in stats.values()):
            raise RuntimeError("No camera frames arrived")
        for stat in stats.values():
            span = (stat["lastArrival"] or 0) - (stat["firstArrival"] or 0)
            stat["arrivalFps"] = (stat["frames"] - 1) / span if span > 0 else 0
        result = {"elapsedSeconds": time.monotonic() - start, "generations": stats}
        (directory / "summary.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        return result
    finally:
        stopped.set()
        try:
            client.command("stop")
        except (OSError, AttributeError):
            pass
        client.close()
        reader.join(timeout=3)
        if not reader.is_alive():
            metadata_file.close()
        for file in files.values():
            file.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="List positively identified USB transports")
    parser.add_argument("--serial", help="Choose one USB device when several are attached")
    parser.add_argument("--launch", action="store_true", help="Open the installed phone app before connecting")
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--output", type=Path, help="New capture directory; defaults to local app data")
    parser.add_argument("--interactive", action="store_true", help="Read JSON controls from stdin")
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("--seconds must be positive and finite")
    client = None
    try:
        adb = AdbServer()
        devices = adb.devices()
        if args.list:
            print(json.dumps(devices, indent=2))
            return 0
        candidates = [device for device in devices if device["state"] == "device" and
                      (args.serial is None or device["serial"] == args.serial)]
        if len(candidates) != 1:
            raise RuntimeError("Connect and authorize one USB phone, or select its USB serial with --serial")
        serial = candidates[0]["serial"]
        if args.launch:
            adb.launch(serial)
            time.sleep(1)
        client = PhoneCameraClient(adb, serial)
        capabilities = client.connect()
        # The binding token stays local to the sockets and is omitted from user-visible logs.
        print(json.dumps({key: value for key, value in capabilities.items() if key != "token"}, indent=2))
        root = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / ".local" / "share"))) / "OkuFlow" / "camera-probes"
        directory = args.output or root / datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        result = capture(client, directory, args.seconds, args.interactive)
        print(json.dumps(result, indent=2))
        print(f"Capture: {directory}")
        return 0
    except (OSError, EOFError, ValueError, RuntimeError, KeyError) as exception:
        print(f"Camera probe failed: {exception}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if client:
            client.close()


if __name__ == "__main__":
    raise SystemExit(main())
