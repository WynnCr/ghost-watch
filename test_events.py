import socket, os, time, json
sock_path = "/tmp/niri-mock.sock"
if os.path.exists(sock_path): os.remove(sock_path)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(sock_path)
s.listen(1)
print("Listening on", sock_path)
conn, _ = s.accept()
print("Connected")
data = conn.recv(1024)
print("Received:", data)
def send(ev):
    conn.sendall(json.dumps(ev).encode() + b'\n')
    time.sleep(0.1)

# Handshake / Initial
send({"WindowsChanged": {"windows": [{"id": 1, "app_id": "firefox", "title": "Mozilla Firefox"}]}})
send({"WindowFocusChanged": {"id": 1}})
print("Sent focus firefox")
time.sleep(2)
send({"WindowFocusChanged": {"id": 2}}) # unknown window
send({"WindowOpenedOrChanged": {"window": {"id": 2, "app_id": "alacritty", "title": "Terminal"}}})
print("Sent focus alacritty")
time.sleep(2)
