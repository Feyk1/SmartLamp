"""CRC-16/CCITT-FALSE for the driver's custom sequences.

The driver computes the same CRC over the 8 registers of each step (low byte
and then high) when it receives `CMD_START_SEQUENCE`; if it does not match, it
sets `STATUS_SEQUENCE_INVALID` and `FAULT_SEQ_INVALID`.
"""

from __future__ import annotations

import struct


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE (init 0xFFFF, polynomial 0x1021, no reflection)."""
    value = crc
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            if value & 0x8000:
                value = ((value << 1) ^ 0x1021) & 0xFFFF
            else:
                value = (value << 1) & 0xFFFF
    return value


def sequence_crc(registers: list[int]) -> int:
    """CRC over 16-bit registers in little-endian order."""
    data = b"".join(struct.pack("<H", value & 0xFFFF) for value in registers)
    return crc16_ccitt(data)
