#!/usr/bin/env python3
"""
Binary Protocol Test Tool for GB Remote Firmware

This script provides a command-line interface to test the binary protocol
implementation of the USB serial handler.

Usage:
    python3 test_binary_protocol.py /dev/ttyACM0
    python3 test_binary_protocol.py COM3  # Windows
"""

import serial
import struct
import sys
import time
from typing import Optional, Dict, Any

class BinaryProtocol:
    # Protocol constants
    START_BYTE = 0xAA
    PACKET_MAX_PAYLOAD_SIZE = 512

    # Command IDs
    CMD_PING = 0x01
    CMD_GET_FIRMWARE_VERSION = 0x02
    CMD_GET_CONFIG = 0x03
    CMD_GET_CALIBRATION = 0x04
    CMD_CALIBRATE_THROTTLE = 0x05
    CMD_RESET_ODOMETER = 0x06
    CMD_SET_SPEED_UNIT = 0x07
    CMD_SET_BACKLIGHT = 0x08
    CMD_INVERT_THROTTLE = 0x09
    CMD_START_STREAMING = 0x10
    CMD_STOP_STREAMING = 0x11
    CMD_SET_STREAM_RATE = 0x12

    # Response IDs
    RSP_ACK = 0x80
    RSP_ERROR = 0x81
    RSP_FIRMWARE_VERSION = 0x82
    RSP_CONFIG = 0x83
    RSP_CALIBRATION = 0x84
    RSP_STREAM_DATA = 0x90

    # Error codes
    ERR_OK = 0x00
    ERR_UNKNOWN_CMD = 0x01
    ERR_INVALID_PAYLOAD = 0x02
    ERR_CRC_MISMATCH = 0x03
    ERR_CALIBRATION_FAILED = 0x04
    ERR_SAVE_FAILED = 0x05
    ERR_NOT_CALIBRATED = 0x06
    ERR_OUT_OF_RANGE = 0x07
    ERR_NOT_SUPPORTED = 0x08

    ERROR_NAMES = {
        0x00: "OK",
        0x01: "UNKNOWN_CMD",
        0x02: "INVALID_PAYLOAD",
        0x03: "CRC_MISMATCH",
        0x04: "CALIBRATION_FAILED",
        0x05: "SAVE_FAILED",
        0x06: "NOT_CALIBRATED",
        0x07: "OUT_OF_RANGE",
        0x08: "NOT_SUPPORTED",
    }

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 1.0):
        """Initialize serial connection."""
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        print(f"Connected to {port} at {baudrate} baud")
        time.sleep(0.5)  # Give device time to initialize

    def calc_crc16(self, data: bytes) -> int:
        """Calculate CRC-16-CCITT over data."""
        crc = 0xFFFF
        for byte in data:
            crc ^= (byte << 8)
            for _ in range(8):
                if crc & 0x8000:
                    crc = (crc << 1) ^ 0x1021
                else:
                    crc = crc << 1
            crc &= 0xFFFF
        return crc

    def send_packet(self, cmd_id: int, payload: bytes = b'') -> None:
        """Send a binary protocol packet."""
        length = len(payload)

        if length > self.PACKET_MAX_PAYLOAD_SIZE:
            raise ValueError(f"Payload too large: {length} bytes")

        # Build packet without CRC
        packet = bytearray()
        packet.append(self.START_BYTE)
        packet.append(cmd_id)
        packet.append(length & 0xFF)
        packet.append((length >> 8) & 0xFF)
        packet.extend(payload)

        # Calculate CRC over CMD + LEN + PAYLOAD
        crc_data = packet[1:]
        crc = self.calc_crc16(crc_data)

        # Append CRC
        packet.append(crc & 0xFF)
        packet.append((crc >> 8) & 0xFF)

        self.ser.write(packet)
        print(f"TX [{len(packet)}]: {packet.hex(' ')}")

    def read_packet(self, timeout: Optional[float] = None) -> Optional[Dict[str, Any]]:
        """Read a binary protocol packet."""
        if timeout is not None:
            old_timeout = self.ser.timeout
            self.ser.timeout = timeout

        try:
            # Wait for start byte
            start_time = time.time()
            while True:
                byte = self.ser.read(1)
                if not byte:
                    return None
                if byte[0] == self.START_BYTE:
                    break
                if timeout and (time.time() - start_time) > timeout:
                    return None

            # Read header
            header = self.ser.read(3)
            if len(header) < 3:
                return None

            cmd = header[0]
            length = struct.unpack('<H', header[1:3])[0]

            # Validate length
            if length > self.PACKET_MAX_PAYLOAD_SIZE:
                print(f"Invalid payload length: {length}")
                return None

            # Read payload
            payload = self.ser.read(length) if length > 0 else b''
            if len(payload) < length:
                return None

            # Read CRC
            crc_bytes = self.ser.read(2)
            if len(crc_bytes) < 2:
                return None

            crc_received = struct.unpack('<H', crc_bytes)[0]

            # Verify CRC
            crc_data = bytes([cmd]) + header[1:3] + payload
            crc_calculated = self.calc_crc16(crc_data)

            if crc_calculated != crc_received:
                print(f"❌ CRC mismatch! Expected {crc_calculated:04X}, got {crc_received:04X}")
                return None

            full_packet = bytes([self.START_BYTE, cmd]) + header[1:3] + payload + crc_bytes
            print(f"RX [{len(full_packet)}]: {full_packet.hex(' ')}")

            return {'cmd': cmd, 'payload': payload}

        finally:
            if timeout is not None:
                self.ser.timeout = old_timeout

    def ping(self) -> bool:
        """Ping the device."""
        print("\n=== PING ===")
        self.send_packet(self.CMD_PING)
        resp = self.read_packet()

        if resp and resp['cmd'] == self.RSP_ACK:
            orig_cmd, error_code = struct.unpack('BB', resp['payload'])
            success = (error_code == self.ERR_OK)
            print(f"✓ Ping {'successful' if success else 'failed'}: {self.ERROR_NAMES.get(error_code, 'UNKNOWN')}")
            return success

        print("❌ No response")
        return False

    def get_firmware_version(self) -> Optional[Dict[str, Any]]:
        """Get firmware version."""
        print("\n=== GET FIRMWARE VERSION ===")
        self.send_packet(self.CMD_GET_FIRMWARE_VERSION)
        resp = self.read_packet()

        if resp and resp['cmd'] == self.RSP_FIRMWARE_VERSION:
            payload = resp['payload']
            idx = 0

            major, minor, patch = struct.unpack_from('BBB', payload, idx)
            idx += 3

            build_len = payload[idx]
            idx += 1
            build_str = payload[idx:idx+build_len].decode('utf-8')
            idx += build_len

            model_len = payload[idx]
            idx += 1
            model_str = payload[idx:idx+model_len].decode('utf-8')
            idx += model_len

            idf_len = payload[idx]
            idx += 1
            idf_str = payload[idx:idx+idf_len].decode('utf-8')

            result = {
                'version': f"{major}.{minor}.{patch}",
                'build': build_str,
                'model': model_str,
                'idf_version': idf_str
            }

            print(f"✓ Firmware: v{result['version']}")
            print(f"  Model: {result['model']}")
            print(f"  Build: {result['build']}")
            print(f"  IDF: {result['idf_version']}")

            return result

        print("❌ No response")
        return None

    def get_config(self) -> Optional[Dict[str, Any]]:
        """Get device configuration."""
        print("\n=== GET CONFIG ===")
        self.send_packet(self.CMD_GET_CONFIG)
        resp = self.read_packet()

        if resp and resp['cmd'] == self.RSP_CONFIG:
            payload = resp['payload']
            idx = 0

            flags = payload[idx]
            idx += 1

            backlight = payload[idx]
            idx += 1

            motor_poles = payload[idx]
            idx += 1

            gear_ratio_x1000 = struct.unpack_from('<H', payload, idx)[0]
            idx += 2

            wheel_diam = struct.unpack_from('<H', payload, idx)[0]
            idx += 2

            speed = struct.unpack_from('<i', payload, idx)[0]
            idx += 4

            result = {
                'speed_unit_mph': bool(flags & 0x01),
                'throttle_inverted': bool(flags & 0x02),
                'ble_connected': bool(flags & 0x04),
                'calibrated': bool(flags & 0x08),
                'backlight': backlight,
                'motor_poles': motor_poles,
                'gear_ratio': gear_ratio_x1000 / 1000.0,
                'wheel_diameter_mm': wheel_diam,
                'current_speed': speed
            }

            print(f"✓ Configuration:")
            print(f"  Speed unit: {'MPH' if result['speed_unit_mph'] else 'KM/H'}")
            print(f"  Backlight: {result['backlight']}%")
            print(f"  BLE connected: {result['ble_connected']}")
            print(f"  Calibrated: {result['calibrated']}")
            print(f"  Motor poles: {result['motor_poles']}")
            print(f"  Gear ratio: {result['gear_ratio']:.3f}")
            print(f"  Wheel diameter: {result['wheel_diameter_mm']}mm")
            print(f"  Current speed: {result['current_speed']}")

            return result

        print("❌ No response")
        return None

    def set_backlight(self, brightness: int) -> bool:
        """Set backlight brightness (0-100)."""
        print(f"\n=== SET BACKLIGHT ({brightness}%) ===")

        if brightness < 0 or brightness > 100:
            print("❌ Brightness must be 0-100")
            return False

        payload = struct.pack('B', brightness)
        self.send_packet(self.CMD_SET_BACKLIGHT, payload)
        resp = self.read_packet()

        if resp and resp['cmd'] == self.RSP_ACK:
            orig_cmd, error_code = struct.unpack('BB', resp['payload'])
            success = (error_code == self.ERR_OK)
            print(f"{'✓' if success else '❌'} Set backlight: {self.ERROR_NAMES.get(error_code, 'UNKNOWN')}")
            return success

        print("❌ No response")
        return False

    def start_streaming(self, rate_hz: int = 10) -> bool:
        """Start real-time data streaming."""
        print(f"\n=== START STREAMING ({rate_hz} Hz) ===")

        if rate_hz < 1 or rate_hz > 100:
            print("❌ Rate must be 1-100 Hz")
            return False

        payload = struct.pack('<H', rate_hz)
        self.send_packet(self.CMD_START_STREAMING, payload)
        resp = self.read_packet()

        if resp and resp['cmd'] == self.RSP_ACK:
            orig_cmd, error_code = struct.unpack('BB', resp['payload'])
            success = (error_code == self.ERR_OK)
            print(f"{'✓' if success else '❌'} Streaming started: {self.ERROR_NAMES.get(error_code, 'UNKNOWN')}")
            return success

        print("❌ No response")
        return False

    def stop_streaming(self) -> bool:
        """Stop real-time data streaming."""
        print(f"\n=== STOP STREAMING ===")
        self.send_packet(self.CMD_STOP_STREAMING)
        resp = self.read_packet()

        if resp and resp['cmd'] == self.RSP_ACK:
            orig_cmd, error_code = struct.unpack('BB', resp['payload'])
            success = (error_code == self.ERR_OK)
            print(f"{'✓' if success else '❌'} Streaming stopped: {self.ERROR_NAMES.get(error_code, 'UNKNOWN')}")
            return success

        print("❌ No response")
        return False

    def parse_stream_data(self, payload: bytes) -> Dict[str, Any]:
        """Parse stream data packet."""
        idx = 0

        timestamp = struct.unpack_from('<I', payload, idx)[0]
        idx += 4

        flags = payload[idx]
        idx += 1

        speed = struct.unpack_from('<i', payload, idx)[0]
        idx += 4

        throttle_raw = struct.unpack_from('<I', payload, idx)[0]
        idx += 4

        throttle_pct = payload[idx]
        idx += 1

        return {
            'timestamp_ms': timestamp,
            'ble_connected': bool(flags & 0x01),
            'calibrated': bool(flags & 0x02),
            'speed': speed,
            'throttle_raw': throttle_raw,
            'throttle_percent': throttle_pct
        }

    def monitor_stream(self, duration_sec: float = 10.0) -> None:
        """Monitor streaming data for specified duration."""
        print(f"\n=== MONITORING STREAM ({duration_sec}s) ===")
        print("Press Ctrl+C to stop\n")

        start_time = time.time()
        packet_count = 0

        try:
            while (time.time() - start_time) < duration_sec:
                packet = self.read_packet(timeout=0.5)

                if packet and packet['cmd'] == self.RSP_STREAM_DATA:
                    data = self.parse_stream_data(packet['payload'])
                    packet_count += 1

                    print(f"[{packet_count:4d}] T={data['timestamp_ms']:8d}ms | "
                          f"Speed={data['speed']:4d} | "
                          f"Throttle={data['throttle_percent']:3d}% (raw={data['throttle_raw']:5d}) | "
                          f"BLE={'✓' if data['ble_connected'] else '✗'} | "
                          f"Cal={'✓' if data['calibrated'] else '✗'}")

        except KeyboardInterrupt:
            print("\n\nStopped by user")

        elapsed = time.time() - start_time
        rate = packet_count / elapsed if elapsed > 0 else 0
        print(f"\n✓ Received {packet_count} packets in {elapsed:.2f}s (avg rate: {rate:.1f} Hz)")

    def close(self):
        """Close serial connection."""
        if self.ser.is_open:
            self.ser.close()
            print("\nConnection closed")


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 test_binary_protocol.py <serial_port>")
        print("Example: python3 test_binary_protocol.py /dev/ttyACM0")
        sys.exit(1)

    port = sys.argv[1]

    try:
        proto = BinaryProtocol(port)

        # Test basic commands
        if not proto.ping():
            print("\n❌ Device not responding. Check connection.")
            return

        proto.get_firmware_version()
        proto.get_config()

        # Test backlight
        proto.set_backlight(50)
        time.sleep(0.5)
        proto.set_backlight(100)

        # Test streaming
        if proto.start_streaming(rate_hz=20):
            proto.monitor_stream(duration_sec=5.0)
            proto.stop_streaming()

        print("\n✓ All tests completed!")

    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
    except Exception as e:
        print(f"\n❌ Error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        try:
            proto.close()
        except:
            pass


if __name__ == '__main__':
    main()

