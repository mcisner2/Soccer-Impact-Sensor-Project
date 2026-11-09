"""Commands the app writes to the Control characteristic.

Every command is exactly 2 bytes: [command code, argument]. Use argument 0 when the
command has none. The firmware ignores anything that is not exactly 2 bytes.
"""
from enum import IntEnum


class Command(IntEnum):
    START_CAL = 0x01
    START_SESSION = 0x02
    STOP_SESSION = 0x03
    RECAL = 0x04
    RAW_STREAM_ON = 0x05
    RAW_STREAM_OFF = 0x06


def encode_command(command: Command, arg: int = 0) -> bytes:
    if not 0 <= arg <= 0xFF:
        raise ValueError(f"command argument must fit in one byte (0-255), got {arg}")
    return bytes([int(Command(command)), arg])
