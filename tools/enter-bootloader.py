#!/usr/bin/env python3
"""Trigger ButteRFly bootloader mode via WHAD DeviceReset."""
import sys
import time
import serial

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    
    # WHAD DeviceReset message: preamble + length + type + domain + type
    # preamble: AC BE
    # header: 00 01 (version 1.0) 00 06 (len=6) 00 00 (dst=generic) 
    # msg type: 0001 (discovery) 0001 (device_reset)
    # Actually, use the whad library to craft this properly
    
    try:
        from whad.protocol.whad_pb2 import DiscoveryMessage, Message, message
        from whad import Hub
        
        ser = serial.Serial(port, 115200, timeout=1)
        
        hub = Hub()
        reset_msg = hub.create_discovery_message(
            DiscoveryMessage.MessageType.DEVICE_RESET
        )
        
        raw = reset_msg.to_bytes()
        ser.write(raw)
        ser.close()
        
        print(f"Sent DeviceReset to {port}")
        time.sleep(0.5)
        print("Device should now be in bootloader mode")
        
    except ImportError:
        # Fallback: craft raw WHAD packet manually
        # WHAD preamble: AC BE
        # Header: version=1, type=0x0000 (generic)
        # Discovery reset command
        ser = serial.Serial(port, 115200, timeout=1)
        
        # Raw bytes for a WHAD discovery device reset message
        # This is the minimal valid WHAD packet for DeviceReset
        pkt = bytes([
            0xAC, 0xBE,  # preamble
            0x00, 0x01,  # version 1.0
            0x00, 0x00,  # dst: generic
            0x00, 0x00,  # src: host
            0x00, 0x06,  # id + .type(0=generic) + .result(0=success)
            0x00, 0x06,  # payload length  
            0x08, 0x01,  # protobuf: field 1 (msg_type), varint, value 1 (device_reset)
        ])
        
        ser.write(pkt)
        ser.close()
        
        print(f"Sent raw DeviceReset to {port}")
        time.sleep(0.5)
        print("Device should now be in bootloader mode")

if __name__ == "__main__":
    main()
