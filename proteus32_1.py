
import argparse
import os
import serial
import struct
import sys
import time
from tqdm import tqdm

VERSION = "1.1"

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


def wait_response(ser, timeout=2.0):
    response = bytearray()
    start = time.time()

    while time.time() - start < timeout:
        if ser.in_waiting:
            response.extend(ser.read(1))
            if len(response) >= 6:
                break
        time.sleep(0.001)

    return (
        len(response) >= 6 and
        response[0] == SYNC_HEAD1 and
        response[1] == SYNC_HEAD2 and
        response[3] == 0
    )


def send_packet(ser, packet, expect_response=True):
    ser.write(packet)
    ser.flush()
    return wait_response(ser) if expect_response else True


def flash_firmware(port, filepath, baudrate):
    with open(filepath, "rb") as f:
        data = f.read()

    print("=" * 40)
    print("CH32 IAP Programmer")
    print(f"Version   : {VERSION}")
    print("=" * 40)
    print(f"Port      : {port}")
    print(f"Baudrate  : {baudrate}")
    print(f"Firmware  : {os.path.basename(filepath)}")
    print(f"Size      : {len(data)} bytes\n")

    ser = serial.Serial(
        port=port,
        baudrate=baudrate,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=2,
    )

    try:
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        print("Erasing flash...")
        if not send_packet(ser, make_packet(CMD_IAP_ERASE, addr=FLASH_BASE)):
            print("ERROR: Flash erase failed.")
            return False
        print("OK\n")

        time.sleep(0.1)

        print("Programming...")

        start = time.time()

        with tqdm(
            total=len(data),
            unit="B",
            unit_scale=True,
            unit_divisor=1024,
            desc="Flashing",
            dynamic_ncols=True,
        ) as pbar:

            for offset in range(0, len(data), BLOCK_SIZE):
                chunk = data[offset:offset + BLOCK_SIZE]

                if not send_packet(ser, make_packet(CMD_IAP_PROM, data=chunk)):
                    print(f"\nERROR: Programming failed at offset 0x{offset:08X}")
                    return False

                pbar.update(len(chunk))

        elapsed = max(time.time() - start, 1e-6)
        speed = len(data) / elapsed / 1024

        print(f"\nProgramming completed ({speed:.1f} KiB/s)")

        print("Finalizing...")

        if not send_packet(ser, make_packet(CMD_IAP_VERIFY, addr=FLASH_BASE)):
            print("ERROR: Verify failed.")
            return False

        time.sleep(0.1)

        send_packet(ser, make_packet(CMD_IAP_END), expect_response=False)
        time.sleep(0.5)

        print("Done.")
        print("Target MCU restarted.")
        return True

    finally:
        ser.close()


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
        return 0 if flash_firmware(
            args.port,
            args.firmware,
            args.baudrate,
        ) else 1

    except serial.SerialException as e:
        print(f"Serial error: {e}")
        return 1

    except KeyboardInterrupt:
        print("\nCancelled by user.")
        return 1

    except Exception as e:
        print(f"Unexpected error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
