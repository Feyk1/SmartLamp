"""Minimal Modbus TCP client (FC01/02/03/04/05/06/16) with no dependencies.

The Simulation Panel uses it to read/write driver registers and to
inject faults through the `HOLDING_SIM_*` registers.
"""

from __future__ import annotations

import socket
import struct

MBAP_SIZE = 7


class ModbusError(Exception):
    """Protocol or connection error with the driver."""


class ModbusClient:
    def __init__(
        self,
        host: str,
        port: int,
        unit: int = 1,
        timeout: float = 2.0,
    ) -> None:
        self.host = host
        self.port = port
        self.unit = unit
        self.timeout = timeout
        self._tid = 0

    def _next_tid(self) -> int:
        self._tid = (self._tid + 1) & 0xFFFF
        return self._tid

    @staticmethod
    def _recv_exact(sock: socket.socket, size: int) -> bytes:
        data = b""
        while len(data) < size:
            chunk = sock.recv(size - len(data))
            if not chunk:
                raise ModbusError("connection closed by driver")
            data += chunk
        return data

    def _transact(self, pdu: bytes) -> bytes:
        request = (
            struct.pack(">HHHB", self._next_tid(), 0, len(pdu) + 1, self.unit)
            + pdu
        )
        try:
            with socket.create_connection(
                (self.host, self.port), self.timeout
            ) as sock:
                sock.settimeout(self.timeout)
                sock.sendall(request)
                header = self._recv_exact(sock, MBAP_SIZE)
                _tid, _proto, length, _unit = struct.unpack(">HHHB", header)
                body = self._recv_exact(sock, length - 1)
        except OSError as exc:
            raise ModbusError(f"driver unreachable: {exc}") from exc

        if not body:
            raise ModbusError("empty response")
        if body[0] & 0x80:
            code = body[1] if len(body) > 1 else 0
            raise ModbusError(f"Modbus exception {code}")
        return body

    @staticmethod
    def _pack_bits(bits: list[int]) -> list[int]:
        return [1 if bit else 0 for bit in bits]

    @staticmethod
    def _unpack_bits(data: bytes, count: int) -> list[int]:
        bits = []
        for i in range(count):
            byte = data[i // 8]
            bits.append((byte >> (i % 8)) & 1)
        return bits

    def read_bits(self, function: int, addr: int, count: int) -> list[int]:
        if count < 1 or count > 2000:
            raise ModbusError("count out of range")
        body = self._transact(struct.pack(">BHH", function, addr, count))
        return self._unpack_bits(body[2:], count)

    def read_words(self, function: int, addr: int, count: int) -> list[int]:
        if count < 1 or count > 125:
            raise ModbusError("count out of range")
        body = self._transact(struct.pack(">BHH", function, addr, count))
        byte_count = body[1] if len(body) > 1 else 0
        values = struct.unpack(f">{byte_count // 2}H", body[2 : 2 + byte_count])
        return [int(value) for value in values]

    def read_coils(self, addr: int, count: int) -> list[int]:
        return self.read_bits(0x01, addr, count)

    def read_discrete(self, addr: int, count: int) -> list[int]:
        return self.read_bits(0x02, addr, count)

    def read_holding(self, addr: int, count: int) -> list[int]:
        return self.read_words(0x03, addr, count)

    def read_input(self, addr: int, count: int) -> list[int]:
        return self.read_words(0x04, addr, count)

    def write_coil(self, addr: int, value: int) -> None:
        payload = 0xFF00 if value else 0x0000
        self._transact(struct.pack(">BHH", 0x05, addr, payload))

    def write_register(self, addr: int, value: int) -> None:
        if value < 0 or value > 0xFFFF:
            raise ModbusError("value out of range")
        self._transact(struct.pack(">BHH", 0x06, addr, value))

    def write_registers(self, addr: int, values: list[int]) -> None:
        if not 1 <= len(values) <= 123:
            raise ModbusError("register count out of range")
        pdu = struct.pack(">BHHB", 0x10, addr, len(values), len(values) * 2)
        pdu += struct.pack(f">{len(values)}H", *values)
        self._transact(pdu)
