"""pytest vectors for proteus32_3.py (protocol rev 2, TZ BOOTLOADER_TZ.md 3.5.4).

Control frames are compared byte-for-byte with the spec, including the
little-endian 16-bit checksum. RUN:  pytest test/test_protocol_v2.py -v
"""

import pytest
import sys

import proteus32_protocol as p

SYNC1 = 0xAA
SYNC2 = 0x55


def base_response(status):
    return bytes([SYNC1, SYNC2, 0x00, status, SYNC2, SYNC1])


def read_frame(addr, data64):
    ck = (p.CMD_IAP_READ + 0x00 + sum(data64)) & 0xFFFF
    return bytes([SYNC1, SYNC2, p.CMD_IAP_READ, 0x00]) + bytes(data64) + \
        bytes([ck & 0xFF, (ck >> 8) & 0xFF, SYNC2, SYNC1])


# ---------------------------------------------------------------- vectors 3.5.4

def test_erase_vector_byte_exact():
    """> AA 55 81 00 00 00 00 08 89 00 55 AA  (CK = 0x0089)"""
    frame = p.make_packet(p.CMD_IAP_ERASE, addr=0x08000000)
    assert frame == bytes([SYNC1, SYNC2, 0x81, 0x00, 0x00, 0x00, 0x00, 0x08,
                           0x89, 0x00, SYNC2, SYNC1])


def test_program_vector_byte_exact():
    """> AA 55 80 40 FF..FF(64) 80 40 55 AA  (CK = 0x4080)"""
    blob = b"\xFF" * 64
    frame = p.make_packet(p.CMD_IAP_PROM, data=blob)
    assert frame == bytes([SYNC1, SYNC2, 0x80, 0x40]) + blob + \
        bytes([0x80, 0x40, SYNC2, SYNC1])


def test_verify_vector_byte_exact():
    """> AA 55 82 40 00 00 00 08 FF..FF(64) 8A 40 55 AA  (CK = 0x408A)"""
    blob = b"\xFF" * 64
    frame = p.make_packet(p.CMD_IAP_VERIFY, addr=0x08000000, data=blob)
    assert frame[:8] == bytes([SYNC1, SYNC2, 0x82, 0x40, 0x00, 0x00, 0x00, 0x08])
    assert frame[8:72] == blob
    assert frame[72:] == bytes([0x8A, 0x40, SYNC2, SYNC1])


def test_end_vector_byte_exact():
    """> AA 55 83 00 83 00 55 AA  (CK = 0x0083)"""
    frame = p.make_packet(p.CMD_IAP_END)
    assert frame == bytes([SYNC1, SYNC2, 0x83, 0x00, 0x83, 0x00, SYNC2, SYNC1])


def test_read_vector_byte_exact():
    """> AA 55 84 04 00 3F 00 08 CF 00 55 AA  (CK = 0x00CF)"""
    frame = p.make_packet(p.CMD_IAP_READ, addr=0x08003F00)
    assert frame == bytes([SYNC1, SYNC2, 0x84, 0x04, 0x00, 0x3F, 0x00, 0x08,
                           0xCF, 0x00, SYNC2, SYNC1])


# ------------------------------------------------------------- frame structure

def test_packet_len_field():
    """LEN field equals the payload length: PROM/VERIFY=0x40, READ=4."""
    assert p.make_packet(p.CMD_IAP_PROM, data=b"\x11" * 64)[3] == 0x40
    assert p.make_packet(p.CMD_IAP_VERIFY, addr=0, data=b"\x00" * 64)[3] == 0x40
    assert p.make_packet(p.CMD_IAP_READ, addr=0)[3] == 4
    assert p.make_packet(p.CMD_IAP_ERASE)[3] == 0
    assert p.make_packet(p.CMD_IAP_END)[3] == 0


def test_packet_checksum():
    """Arbitrary payload: CK on the host must equal CK computed over the frame."""
    import random
    random.seed(42)
    for _ in range(200):
        blob = bytes(random.getrandbits(8) for _ in range(64))
        frame = p.make_packet(p.CMD_IAP_PROM, data=blob)
        host_ck = (p.CMD_IAP_PROM + 0x40 + sum(blob)) & 0xFFFF
        assert frame[4 + 64] == host_ck & 0xFF
        assert frame[5 + 64] == (host_ck >> 8) & 0xFF


def test_checksum_wrap():
    """CK wraps on 16 bits (sum 0x1FFF0 -> 0xFFF0)."""
    frame = p.make_packet(p.CMD_IAP_PROM, data=b"\xFF" * 64)
    assert frame[68] == 0x80 and frame[69] == 0x40


def test_make_packet_unknown_cmd():
    with pytest.raises(ValueError):
        p.make_packet(0xEE)


# ------------------------------------------------------------- response parsing

def test_parse_ok_response():
    ok, status = p.parse_base_response(base_response(0x00))
    assert ok is True and status == 0x00


def test_parse_error_statuses():
    for code in (0x01, 0x02, 0x03):
        ok, status = p.parse_base_response(base_response(code))
        assert ok is False
        assert status == code


def test_parse_garbage_returns_none_status():
    ok, status = p.parse_base_response(b"\xAA\xAA\xAA\xAA\xAA\xAA")
    assert ok is False and status is None


def test_parse_short_response():
    ok, status = p.parse_base_response(b"\xAA\x55")
    assert ok is False and status is None


# -------------------------------------------------------------- status mapping

def test_status_dict():
    assert p.ST_DICT[p.ST_ERR_RANGE] == "нарушение границ app-области (защита WL)"
    assert p.ST_DICT[p.ST_ERR_VERIFY] == "VERIFY: несовпадение данных"


# --------------------------------------------------------------- constants

def test_flash_bounds():
    assert p.FLASH_APP_END - p.FLASH_BASE == 0x3F00          # 16128
    assert p.FLASH_USER_END - p.FLASH_APP_END == 0x100       # WL_FLASH 256 B
    assert p.MAX_FLASH_SIZE == 16128
    assert p.BLOCK_SIZE == 64


def test_cli_module_reexports_protocol():
    """proteus32_3.py re-exports the shared protocol layer for its CLI."""
    import proteus32_3
    for name in ("BLOCK_SIZE", "FLASH_BASE", "FLASH_USER_END", "MAX_FLASH_SIZE",
                 "ST_DICT", "ST_ERR_RANGE", "ST_ERR_VERIFY", "make_packet"):
        assert hasattr(proteus32_3, name)


def test_no_force_flag():
    """--force must be gone: the app-area limit is structural (TZ 4.2)."""
    import subprocess
    out = subprocess.run(
        [sys.executable, "proteus32_3.py", "--help"],
        capture_output=True, text=True,
    )
    assert out.returncode == 0
    assert "--force" not in out.stdout
    assert "--dump" in out.stdout and "--erase-only" in out.stdout and "--go" in out.stdout


def test_version_prints_protocol_rev():
    import subprocess
    out = subprocess.run(
        [sys.executable, "proteus32_3.py", "--version"],
        capture_output=True, text=True,
    )
    assert out.returncode == 0
    assert "2.1" in out.stdout
    assert "protocol rev 2" in out.stdout