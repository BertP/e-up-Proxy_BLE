import serial
import time
import sys

port = "/dev/ttyACM0"
baud = 115200

print(f"Monitoring serial from {port} for 90 seconds...")
try:
    ser = serial.Serial(port, baud, timeout=0.5, dsrdtr=False, rtscts=False)
    ser.dtr = False
    ser.rts = False
    
    start_time = time.time()
    lines_read = 0
    while time.time() - start_time < 90:
        line = ser.readline()
        if line:
            print(line.decode("utf-8", errors="ignore").strip(), flush=True)
            lines_read += 1
            
    ser.close()
    print(f"Finished monitoring. Total lines: {lines_read}")
except Exception as e:
    print(f"Error: {e}")
