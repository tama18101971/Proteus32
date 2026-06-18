import argparse
import os
import struct
import sys
import time

import serial
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
    payload = bytearray()
    payload.append(cmd)

    if cmd == CMD_IAP_ERASE:
        payload.append(0x00)
        payload.extend(
            struct.pack(
                "<I",
                addr if addr is not None else FLASH_BASE
            )
        )

    elif cmd == CMD_IAP_PROM:
        payload.append(len(data))
        payload.extend(data)

    elif cmd == CMD_IAP_VERIFY:
        payload.append(0x00)
        payload.extend(
            struct.pack(
                "<I",
                addr if addr is not None else FLASH_BASE
            )
        )

    elif cmd == CMD_IAP_END:
        payload.append(0x00)

    checksum = sum(payload)

    packet = bytearray()

    packet.append(SYNC_HEAD1)
    packet.append(SYNC_HEAD2)

    packet.extend(payload)

    packet.append(checksum & 0xFF)
    packet.append((checksum >> 8) & 0xFF)

    packet.append(SYNC_HEAD2)
    packet.append(SYNC_HEAD1)

    return bytes(packet)


def wait_response(ser, timeout=2.0):
    response = bytearray()

    start = time.time()

    while time.time() - start < timeout:

        if ser.in_waiting:

            byte = ser.read(1)

            if byte:
                response.extend(byte)

                if len(response) >= 6:
                    break

        time.sleep(0.001)

    if len(response) < 6:
        return False

    if response[0] != SYNC_HEAD1:
        return False

    if response[1] != SYNC_HEAD2:
        return False

    return response[3] == 0


def send_packet(ser, packet, expect_response=True):

    ser.write(packet)
    ser.flush()

    if expect_response:
        return wait_response(ser)

    return True


def print_header(port, baudrate, filepath, size_bytes):

    print("=" * 56)
    print(f" CH32 IAP Programmer v{VERSION}")
    print("=" * 56)

    print(f"Port      : {port}")
    print(f"Baudrate  : {baudrate}")
    print(f"Firmware  : {os.path.basename(filepath)}")
    print(f"Size      : {size_bytes / 1024:.1f} KiB")

    print()


def print_summary(elapsed, speed):

    print()
    print("=" * 56)
    print("✓ Flash completed successfully")
    print()

    print(f"Elapsed : {elapsed:.2f} s")
    print(f"Speed   : {speed:.1f} KiB/s")

    print("=" * 56)


def flash_firmware(port, filepath, baudrate):
    with open(filepath, "rb") as f:
        data = f.read()

    print_header(
        port=port,
        baudrate=baudrate,
        filepath=filepath,
        size_bytes=len(data),
    )

    ser = serial.Serial(
        port=port,
        baudrate=baudrate,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=2,
    )

    total_start = time.time()

    try:

        ser.reset_input_buffer()
        ser.reset_output_buffer()

        #
        # Stage 1
        #
        erase_start = time.time()

        print("▶ Erasing flash... ", end="", flush=True)

        if not send_packet(
            ser,
            make_packet(
                CMD_IAP_ERASE,
                addr=FLASH_BASE,
            ),
        ):
            print("FAILED")
            return False

        erase_time = (time.time() - erase_start) * 1000.0

        print(f"OK ({erase_time:.0f} ms)")
        print()

        time.sleep(0.1)

        #
        # Stage 2
        #
        print("▶ Programming")
        print()

        prog_start = time.time()

        with tqdm(
            total=len(data),
            desc="Flashing",
            unit="B",
            unit_scale=True,
            unit_divisor=1024,
            dynamic_ncols=True,
            leave=True,
        ) as pbar:

            for offset in range(
                0,
                len(data),
                BLOCK_SIZE,
            ):

                chunk = data[
                    offset:
                    offset + BLOCK_SIZE
                ]

                packet = make_packet(
                    CMD_IAP_PROM,
                    data=chunk,
                )

                if not send_packet(
                    ser,
                    packet,
                ):
                    print()
                    print(
                        f"Programming failed at "
                        f"offset 0x{offset:08X}"
                    )
                    return False

                pbar.update(len(chunk))

        prog_time = time.time() - prog_start

        print()

        #
        # Stage 3
        #
        print("▶ Finalizing... ", end="", flush=True)

        packet = make_packet(
            CMD_IAP_VERIFY,
            addr=FLASH_BASE,
        )

        if not send_packet(
            ser,
            packet,
        ):
            print("FAILED")
            return False

        time.sleep(0.1)

        packet = make_packet(
            CMD_IAP_END,
        )

        ser.write(packet)
        ser.flush()

        time.sleep(0.5)

        print("OK")

        total_time = time.time() - total_start

        speed = (
            len(data)
            / total_time
            / 1024.0
        )

        print_summary(
            elapsed=total_time,
            speed=speed,
        )

        return True

    finally:
        ser.close()


def main():

    parser = argparse.ArgumentParser(
        description="CH32 IAP Firmware Programmer"
    )
	    parser.add_argument(
        "port",
        help="Serial port (e.g. COM5)",
    )

    parser.add_argument(
        "firmware",
        help="Firmware binary (*.bin)",
    )

    parser.add_argument(
        "-b",
        "--baudrate",
        type=int,
        default=460800,
        help="Baudrate (default: 460800)",
    )

    parser.add_argument(
        "--version",
        action="version",
        version=f"%(prog)s {VERSION}",
    )

    args = parser.parse_args()

    if not os.path.isfile(args.firmware):
        print(f"ERROR: File not found: {args.firmware}")
        return 1

    try:

        ok = flash_firmware(
            port=args.port,
            filepath=args.firmware,
            baudrate=args.baudrate,
        )

        return 0 if ok else 1

    except serial.SerialException as e:

        print(f"Serial error: {e}")
        return 1

    except KeyboardInterrupt:

        print("\nCancelled.")
        return 1

    except Exception as e:

        print(f"Unexpected error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())