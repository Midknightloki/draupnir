import serial
import sys
import time

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

try:
    ser = serial.Serial(port, baud, timeout=1)
    print(f"Connected to {port} at {baud} baud.")
    
    # Read for 300 seconds to catch the crash
    end_time = time.time() + 300
    while time.time() < end_time:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='replace')
            print(line, end='')
        else:
            time.sleep(0.01)
    ser.close()
    print("\nClosed serial connection.")
except Exception as e:
    print(f"Error: {e}")
