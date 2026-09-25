#!/usr/bin/env python3
"""裸客户端测无线探针：直接发 CMSIS-DAP over TCP 命令，看回什么。

用法：python tools\\dap_tcp_probe.py <ip> [port]
"""
import socket
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.48"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4441
SIG = bytes.fromhex("44415000")          # "DAP"


def pkt(payload: bytes) -> bytes:
    return SIG + len(payload).to_bytes(2, "little") + bytes([0x01, 0x00]) + payload


def show(name: str, payload: bytes, sock: socket.socket, timeout: float = 3.0) -> bytes:
    t0 = time.time()
    sock.sendall(pkt(payload))
    sock.settimeout(timeout)
    try:
        hdr = sock.recv(8)
    except socket.timeout:
        print(f"  {name:<28} 超时（{timeout}s 没响应）")
        return b""
    if len(hdr) < 8:
        print(f"  {name:<28} 头不完整：{hdr.hex(' ')}")
        return b""
    sig = int.from_bytes(hdr[0:4], "little")
    ln = int.from_bytes(hdr[4:6], "little")
    typ = hdr[6]
    body = b""
    while len(body) < ln:
        chunk = sock.recv(ln - len(body))
        if not chunk:
            break
        body += chunk
    dt = (time.time() - t0) * 1000
    ok = "OK " if (sig == 0x00504144 and typ == 0x02) else "!! "
    print(f"  {name:<28} {ok}{dt:6.1f} ms  type={typ:#04x} len={ln:3d}  {body.hex(' ')}")
    return body


print(f"连 {HOST}:{PORT} ...")
s = socket.create_connection((HOST, PORT), timeout=5)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
print("已连接。逐条发命令（每条都算一次往返）：")

show("DAP_Info(0xF0 包个数)", bytes([0x00, 0xF0]), s)
show("DAP_Info(0xF1 包大小)", bytes([0x00, 0xF1]), s)
show("DAP_Info(0x02 产品名)", bytes([0x00, 0x02]), s)
show("DAP_Connect(SWD)", bytes([0x02, 0x01]), s)
show("SWJ_Clock(1MHz)", bytes([0x11, 0x40, 0x42, 0x0F, 0x00]), s)
show("SWD_Configure(0)", bytes([0x13, 0x00]), s)
show("SWJ_Sequence(88 位激活)", bytes([0x12, 88]) + bytes([0x9E, 0xE7] + [0xFF] * 8 + [0x00]), s)
show("TransferConfigure", bytes([0x04, 0x00, 0xE8, 0x03, 0x00, 0x00]), s)
show("读 DP IDCODE", bytes([0x05, 0x00, 0x01, 0x02]), s)

t0 = time.time()
N = 20
for _ in range(N):
    show("读 DP IDCODE", bytes([0x05, 0x00, 0x01, 0x02]), s)
dt = time.time() - t0
print(f"\n{N} 次往返共 {dt*1000:.0f} ms → 每次 {dt/N*1000:.2f} ms")

show("DAP_Disconnect", bytes([0x03]), s)
s.close()
print("关闭连接")
