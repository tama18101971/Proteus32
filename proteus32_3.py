import argparse
import os
import serial
import struct
import sys
import time
from tqdm import tqdm

VERSION = "1.2"

SYNC_HEAD1 = 0xAA
SYNC_HEAD2 = 0x55

CMD_IAP_PROM   = 0x80
CMD_IAP_ERASE  = 0x81
CMD_IAP_VERIFY = 0x82
CMD_IAP_END    = 0x83

FLASH_BASE = 0x08000000
BLOCK_SIZE = 64


def make_packet(cmd, addr=None, data=None):
    payload = bytearray([cmd])

    if cmd == CMD_IAP_ERASE:
        payload.append(0)
        payload.extend(struct.pack("<I", addr if addr else FLASH_BASE))
    elif cmd == CMD_IAP_PROM:
        payload.append(len(data))
        payload.extend(data)
    elif cmd == CMD_IAP_VERIFY:
        payload.append(0)
        payload.extend(struct.pack("<I", addr if addr else FLASH_BASE))
    elif cmd == CMD_IAP_END:
        payload.append(0)

    checksum = sum(payload)

    return bytes([
        SYNC_HEAD1,
        SYNC_HEAD2,
        *payload,
        checksum & 0xFF,
        (checksum >> 8) & 0xFF,
        SYNC_HEAD2,
        SYNC_HEAD1,
    ])


def wait_response(ser, expected_len=6):
    """
    Быстрое чтение ответа. Использует встроенный таймаут pySerial,
    избавляя от побайтового чтения и лишних sleep().
    """
    response = ser.read(expected_len)
    
    return (
        len(response) >= expected_len and
        response[0] == SYNC_HEAD1 and
        response[1] == SYNC_HEAD2 and
        response[3] == 0  # Статус (0 обычно означает успех)
    )


def send_packet(ser, packet, expect_response=True):
    ser.write(packet)
    ser.flush()
    return wait_response(ser) if expect_response else True


def flash_firmware(port, filepath, baudrate):
    with open(filepath, "rb") as f:
        data = f.read()

    # Округление данных до размера блока (заполнение 0xFF)
    remainder = len(data) % BLOCK_SIZE
    if remainder > 0:
        padding = BLOCK_SIZE - remainder
        data += b'\xFF' * padding

    print("=" * 56)
    print(f" CH32 IAP Programmer v{VERSION}")
    print("=" * 56)
    print(f"Port      : {port}")
    print(f"Baudrate  : {baudrate}")
    print(f"Firmware  : {os.path.basename(filepath)}")
    print(f"Size      : {len(data)/1024:.1f} KiB (aligned)")
    print()

    # Использование контекстного менеджера `with` гарантирует закрытие порта
    with serial.Serial(
        port=port,
        baudrate=baudrate,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=1.5,  # 1.5 секунды обычно более чем достаточно
    ) as ser:

        total_start = time.time()

        ser.reset_input_buffer()
        ser.reset_output_buffer()

        # ----------------------------------------------------
        # [1/3] Стирание
        # ----------------------------------------------------
        stage_start = time.time()
        print("[1/3] Erasing flash... ", end="", flush=True)

        if not send_packet(ser, make_packet(CMD_IAP_ERASE, addr=FLASH_BASE)):
            print("FAILED")
            return False

        print(f"OK ({(time.time() - stage_start)*1000:.0f} ms)")
        print()
        time.sleep(0.05)

        # ----------------------------------------------------
        # [2/3] Прошивка
        # ----------------------------------------------------
        print("[2/3] Programming")
        print()
        prog_start = time.time()

        with tqdm(
            total=len(data),
            unit="B",
            unit_scale=True,
            unit_divisor=1024,
            desc="Flashing",
            dynamic_ncols=True,
            leave=True,
        ) as pbar:

            for offset in range(0, len(data), BLOCK_SIZE):
                chunk = data[offset:offset+BLOCK_SIZE]

                if not send_packet(ser, make_packet(CMD_IAP_PROM, data=chunk)):
                    print(f"\nProgramming failed at offset 0x{offset:08X}")
                    return False

                pbar.update(len(chunk))

        print()

        # ----------------------------------------------------
        # [3/3] Верификация и завершение
        # ----------------------------------------------------
        print("[3/3] Finalizing... ", end="", flush=True)

        if not send_packet(ser, make_packet(CMD_IAP_VERIFY, addr=FLASH_BASE)):
            print("FAILED")
            return False

        send_packet(ser, make_packet(CMD_IAP_END), expect_response=False)
        time.sleep(0.2)

        print("OK")
        print()

        total_time = time.time() - total_start
        speed = len(data) / total_time / 1024

        print("=" * 56)
        print("SUCCESS")
        print()
        print(f"Elapsed : {total_time:.2f} s")
        print(f"Speed   : {speed:.1f} KiB/s")
        print("=" * 56)

        return True


def main():
    parser = argparse.ArgumentParser(
        description="CH32 IAP Firmware Programmer"
    )
    parser.add_argument("port", help="Serial port (e.g. COM5)")
    parser.add_argument("firmware", help="Firmware binary (*.bin)")
    parser.add_argument(
        "-b", "--baudrate",
        type=int,
        default=460800,
        help="Baudrate (default: 460800)"
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"%(prog)s {VERSION}"
    )

    args = parser.parse_args()

    if not os.path.isfile(args.firmware):
        print(f"ERROR: File not found: {args.firmware}")
        return 1

    try:
        return 0 if flash_firmware(args.port, args.firmware, args.baudrate) else 1
    except serial.SerialException as e:
        print(f"\nSerial error: {e}")
        return 1
    except KeyboardInterrupt:
        print("\nCancelled by user.")
        return 1
    except Exception as e:
        print(f"\nUnexpected error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())