import argparse
import os
import serial
import struct
import sys
import time

VERSION = "1.0"

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

    packet = bytearray()
    packet.append(SYNC_HEAD1)
    packet.append(SYNC_HEAD2)
    packet.extend(payload)
    packet.append(checksum & 0xFF)
    packet.append((checksum >> 8) & 0xFF)
    packet.append(SYNC_HEAD2)
    packet.append(SYNC_HEAD1)

    return bytes(packet)


def wait_response(ser, timeout=2):
    response = bytearray()
    start = time.time()

    while time.time() - start < timeout:
        if ser.in_waiting:
            response.extend(ser.read(1))

            if len(response) >= 6:
                break

        time.sleep(0.001)

    if len(response) < 6:
        return False

    if response[0] != SYNC_HEAD1 or response[1] != SYNC_HEAD2:
        return False

    return response[3] == 0


def send_packet(ser, packet, expect_response=True):
    ser.write(packet)
    ser.flush()

    if expect_response:
        return wait_response(ser)

    return True


def flash_firmware(port, filepath, baudrate):

    with open(filepath, "rb") as f:
        data = f.read()

    print("===================================")
    print("CH32 IAP Programmer")
    print("===================================")
    print(f"Port      : {port}")
    print(f"Baudrate  : {baudrate}")
    print(f"Firmware  : {filepath}")
    print(f"Size      : {len(data)} bytes")
    print()

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

        if not send_packet(
            ser,
            make_packet(CMD_IAP_ERASE, addr=FLASH_BASE),
        ):
            print("ERROR: Flash erase failed.")
            return False

        print("OK")

        time.sleep(0.1)

        print("Programming...")

        total_blocks = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE

        start = time.time()

        for block, offset in enumerate(range(0, len(data), BLOCK_SIZE), start=1):

            chunk = data[offset:offset + BLOCK_SIZE]

            if not send_packet(
                ser,
                make_packet(CMD_IAP_PROM, data=chunk),
            ):
                print(f"\nERROR: Programming failed at block {block}.")
                return False

            percent = block * 100 / total_blocks

            print(
                f"\rProgress: {percent:5.1f}% ({block}/{total_blocks})",
                end="",
                flush=True,
            )

        elapsed = time.time() - start

        print()

        speed = len(data) / elapsed / 1024 if elapsed > 0 else 0

        print(f"Programming completed ({speed:.1f} KB/s)")

        time.sleep(0.1)

        print("Finalizing...")

        if not send_packet(
            ser,
            make_packet(CMD_IAP_VERIFY, addr=FLASH_BASE),
        ):
            print("ERROR: Verify command failed.")
            return False

        time.sleep(0.1)

        ser.write(make_packet(CMD_IAP_END))
        ser.flush()

        time.sleep(0.5)

        print("Done.")
        print("MCU restarted.")

        return True

    finally:
        ser.close()


def main():

    parser = argparse.ArgumentParser(
        description="CH32 IAP Firmware Programmer"
    )

    parser.add_argument(
        "port",
        help="COM port (например COM5)",
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
        help="Serial baudrate (default: 460800)",
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
            args.port,
            args.firmware,
            args.baudrate,
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