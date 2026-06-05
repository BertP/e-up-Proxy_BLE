import serial
import time
import sys

port = "/dev/ttyACM0"
baud = 115200

print(f"Reading serial from {port} for 20 seconds WITHOUT resetting...")
try:
    # Open without toggling DTR/RTS (dsrdtr=False, rtscts=False)
    ser = serial.Serial(port, baud, timeout=1, dsrdtr=False, rtscts=False)
    
    # Do NOT toggle DTR/RTS
    ser.dtr = False
    ser.rts = False
    
    start_time = time.time()
    lines_read = 0
    while time.time() - start_time < 20:
        line = ser.readline()
        if line:
            print(line.decode("utf-8", errors="ignore").strip())
            lines_read += 1
            
    ser.close()
    print(f"Finished reading. Total lines: {lines_read}")
except Exception as e:
    print(f"Error: {e}")
