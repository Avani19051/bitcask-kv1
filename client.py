import socket

# Connect to the kvstore server.
HOST, PORT = "localhost", 6380
s = socket.create_connection((HOST, PORT))
print(f"connected to kvstore server at {HOST}:{PORT}")
print("type commands like: SET city Ludhiana | GET city | DEL city | COMPACT | quit\n")

while True:
    line = input("> ").strip()
    if line.lower() in ("quit", "exit"):
        break
    if not line:
        continue
    s.sendall((line + "\n").encode())     # send the command
    reply = s.recv(4096).decode().strip()  # read the server's reply
    print(reply)

s.close()
