import serial
import struct
import time
import sys

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
        payload.extend(struct.pack('<I', addr if addr else FLASH_BASE))
    elif cmd == CMD_IAP_PROM:
        payload.append(len(data))
        payload.extend(data)
    elif cmd == CMD_IAP_VERIFY:
        payload.append(0x00)
        payload.extend(struct.pack('<I', addr if addr else FLASH_BASE))
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

def wait_response(ser, timeout=2):
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
    
    if len(response) >= 6:
        if response[0] == SYNC_HEAD1 and response[1] == SYNC_HEAD2:
            return response[3] == 0
    return False

def send_packet(ser, packet, expect_response=True):
    ser.write(packet)
    ser.flush()
    if expect_response:
        return wait_response(ser)
    return True

def flash_firmware(port, filepath):
    with open(filepath, 'rb') as f:
        data = f.read()
    
    print(f"Firmware: {filepath} ({len(data)} bytes)")
    
    ser = serial.Serial(
        port=port,
        baudrate=460800,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=2
    )
    
    try:
        print(f"Connected to {port}")
        
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        
        print("Erasing flash...")
        packet = make_packet(CMD_IAP_ERASE, addr=FLASH_BASE)
        if not send_packet(ser, packet):
            print("Erase failed!")
            return False
        print("Flash erased")
        
        time.sleep(0.1)
        
        print("Programming...")
        total = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE
        
        for i in range(0, len(data), BLOCK_SIZE):
            chunk = data[i:i+BLOCK_SIZE]
            packet = make_packet(CMD_IAP_PROM, data=chunk)
            
            if not send_packet(ser, packet):
                print(f"\nError at block {i//BLOCK_SIZE + 1}")
                return False
            
            progress = ((i // BLOCK_SIZE + 1) / total) * 100
            print(f"\rProgress: {progress:.1f}%", end="", flush=True)
        
        print("\nProgramming complete")
        
        time.sleep(0.1)
        
        print("Flushing remaining data...")
        packet = make_packet(CMD_IAP_VERIFY, addr=FLASH_BASE)
        if not send_packet(ser, packet):
            print("Flush failed!")
            return False
        
        time.sleep(0.1)
        
        print("Finishing...")
        packet = make_packet(CMD_IAP_END)
        ser.write(packet)
        ser.flush()
        time.sleep(0.5)
        
        print("Done! MCU will reset to APP.")
        return True
        
    finally:
        ser.close()

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <COM_port> <firmware.bin>")
        print(f"Example: {sys.argv[0]} COM3 firmware.bin")
        sys.exit(1)
    
    port = sys.argv[1]
    filepath = sys.argv[2]
    
    try:
        flash_firmware(port, filepath)
    except serial.SerialException as e:
        print(f"Serial error: {e}")
        sys.exit(1)
    except FileNotFoundError:
        print(f"File not found: {filepath}")
        sys.exit(1)
