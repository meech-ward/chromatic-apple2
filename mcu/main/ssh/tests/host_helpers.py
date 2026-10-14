# SPDX-License-Identifier: MIT
# Test-only OpenSSH clients and malformed-packet helpers.
import os,select,socket,struct,subprocess,threading,time

class Client:
    def __init__(self, port, askpass, password="test-pass-123", **options):
        env = dict(os.environ, SSH_ASKPASS=str(askpass), SSH_ASKPASS_REQUIRE="force",
                   DISPLAY=":0", APPLE2_TEST_PASSWORD=password)
        command = ["ssh", "-F", "/dev/null", "-tt", "-p", str(port), "-o", "ConnectTimeout=5",
                   "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                   "-o", "PreferredAuthentications=password", "-o", "LogLevel=ERROR"]
        for name, value in options.items():
            command.extend(["-o", f"{name}={value}"])
        self.process = subprocess.Popen(command + ["apple@127.0.0.1"], env=env,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.received = b""

    def until(self, marker, timeout=10):
        until = time.monotonic() + timeout
        while marker not in self.received and time.monotonic() < until:
            if select.select([self.process.stdout], [], [], .1)[0]:
                data = os.read(self.process.stdout.fileno(), 65536)
                if not data:
                    break
                self.received += data
        assert marker in self.received, ("SSH output missing", marker[:80], self.received[-200:])

    def send(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def close(self):
        self.process.terminate()
        self.process.communicate(timeout=5)


def check_wire_rejection(port):
    for length in (0, 1, 5, 8192, 0xffffffff):
        with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
            s.recv(128)
            for byte in b"SSH-2.0-test-split\r\n":
                s.sendall(bytes([byte]))
            s.sendall(struct.pack(">I", length))
            reply = s.recv(1024)
            assert reply, ("expected protocol disconnect", length)
        time.sleep(.01)


class CorruptEncryptedPacket:
    """Flip one bit after NEWKEYS; framing length stays intact, GCM must reject."""
    def __init__(self, target_port):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.target_port = target_port
        self.corrupted = False
        self.thread = threading.Thread(target=self.forward, daemon=True)
        self.thread.start()

    def forward(self):
        local, _ = self.listener.accept()
        with local, socket.create_connection(("127.0.0.1", self.target_port), timeout=3) as remote:
            pending = b""
            banner = True
            encrypted = False
            deadline = time.monotonic() + 15
            try:
                while time.monotonic() < deadline:
                    readable, _, _ = select.select([local, remote], [], [], .1)
                    for connection in readable:
                        data = connection.recv(65536)
                        if not data:
                            return
                        if connection is remote:
                            local.sendall(data)
                            continue
                        pending += data
                        if banner and b"\n" in pending:
                            header, pending = pending.split(b"\n", 1)
                            remote.sendall(header + b"\n")
                            banner = False
                        while not banner and len(pending) >= 4:
                            length = struct.unpack(">I", pending[:4])[0] + 4 + (16 if encrypted else 0)
                            if len(pending) < length:
                                break
                            packet, pending = pending[:length], pending[length:]
                            if encrypted and not self.corrupted:
                                packet = packet[:-1] + bytes([packet[-1] ^ 1])
                                self.corrupted = True
                            elif not encrypted and packet[5] == 21:
                                encrypted = True
                            remote.sendall(packet)
            except (ConnectionError, OSError):
                pass

    def close(self):
        self.listener.close()
        self.thread.join(timeout=5)

