"""Modbus TCP client tests against a local server."""

from __future__ import annotations

import socketserver
import struct
import threading

import pytest

from src.sim.modbus import ModbusClient, ModbusError


class _Handler(socketserver.BaseRequestHandler):
    def _recv(self, size: int) -> bytes | None:
        data = b""
        while len(data) < size:
            chunk = self.request.recv(size - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def handle(self) -> None:
        while True:
            header = self._recv(7)
            if header is None:
                return
            tid, _proto, length, unit = struct.unpack(">HHHB", header)
            pdu = self._recv(length - 1)
            if pdu is None:
                return
            func = pdu[0]
            srv = self.server
            try:
                if func in (3, 4):
                    addr, count = struct.unpack(">HH", pdu[1:5])
                    if addr + count > len(srv.registers):
                        raise ValueError
                    body = bytes([func, count * 2]) + struct.pack(
                        f">{count}H", *srv.registers[addr : addr + count]
                    )
                elif func == 6:
                    addr, value = struct.unpack(">HH", pdu[1:5])
                    srv.registers[addr] = value
                    body = pdu
                elif func == 5:
                    addr, value = struct.unpack(">HH", pdu[1:5])
                    srv.coils[addr] = 1 if value else 0
                    body = pdu
                elif func in (1, 2):
                    addr, count = struct.unpack(">HH", pdu[1:5])
                    bits = srv.coils if func == 1 else srv.discrete
                    raw = bytearray((count + 7) // 8)
                    for i in range(count):
                        if bits[addr + i]:
                            raw[i // 8] |= 1 << (i % 8)
                    body = bytes([func, len(raw)]) + bytes(raw)
                elif func == 16:
                    addr, count, byte_count = struct.unpack(">HHB", pdu[1:6])
                    values = struct.unpack(
                        f">{count}H", pdu[6 : 6 + byte_count]
                    )
                    for offset, value in enumerate(values):
                        srv.registers[addr + offset] = value
                    body = pdu[:5]
                else:
                    raise ValueError
            except Exception:
                body = bytes([func | 0x80, 2])
            response = (
                struct.pack(">HHHB", tid, 0, len(body) + 1, unit) + body
            )
            self.request.sendall(response)


@pytest.fixture()
def server():
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), _Handler)
    srv.registers = [0] * 64
    srv.coils = [0] * 16
    srv.discrete = [1] * 16
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    yield srv
    srv.shutdown()
    srv.server_close()


def _client(server) -> ModbusClient:
    return ModbusClient("127.0.0.1", server.server_address[1], unit=1, timeout=1.0)


def test_read_and_write_holding(server):
    client = _client(server)
    assert client.read_holding(0, 2) == [0, 0]

    client.write_register(2, 120)
    assert client.read_holding(0, 3) == [0, 0, 120]

    client.write_registers(5, [10, 20, 30])
    assert client.read_holding(5, 3) == [10, 20, 30]


def test_coils_and_discrete(server):
    client = _client(server)
    client.write_coil(0, 1)
    client.write_coil(3, 1)
    assert client.read_coils(0, 4) == [1, 0, 0, 1]
    assert client.read_discrete(0, 2) == [1, 1]


def test_input_registers(server):
    server.registers[10] = 42
    client = _client(server)
    assert client.read_input(10, 1) == [42]


def test_modbus_exception_is_reported(server):
    client = _client(server)
    with pytest.raises(ModbusError):
        client.read_holding(60, 10)


def test_connection_error(server):
    client = ModbusClient("127.0.0.1", 1, timeout=0.2)
    with pytest.raises(ModbusError):
        client.read_holding(0, 1)
