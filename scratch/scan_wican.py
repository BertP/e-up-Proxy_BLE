import socket

target = "192.168.0.1"
ports = [23, 80, 1883, 8883, 35000, 8266]

print(f"Scanning Wican IP {target} for open TCP ports...")
for port in ports:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    result = s.connect_ex((target, port))
    if result == 0:
        print(f"  -> [OPEN] Port {port} is reachable!")
    else:
        print(f"  -> [CLOSED] Port {port}")
    s.close()
