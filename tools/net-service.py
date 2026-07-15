#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host L3/L4 peer used by slirp, passt and TAP network backends."""

import argparse
import selectors
import signal
import socket
import struct
from pathlib import Path


TCP_PORT = 18080
RX_PORT = 18081
TX_PORT = 18082
BURST_MAGIC = b"LUNABRST"
TX_MAGIC = b"LUNATX25"
TX_ACK_MAGIC = b"LUNATXOK"


class Service:
    def __init__(self, bind: str) -> None:
        self.selector = selectors.DefaultSelector()
        self.running = True
        self.clients: set[socket.socket] = set()
        self.tx_streams: dict[tuple[str, int, int], set[int]] = {}
        self.tx_completed: set[tuple[str, int, int]] = set()

        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind((bind, TCP_PORT))
        self.tcp.listen()
        self.tcp.setblocking(False)
        self.selector.register(self.tcp, selectors.EVENT_READ, self.accept)

        self.rx = self.udp_socket(bind, RX_PORT)
        self.tx = self.udp_socket(bind, TX_PORT)
        self.selector.register(self.rx, selectors.EVENT_READ, self.rx_burst)
        self.selector.register(self.tx, selectors.EVENT_READ, self.tx_stress)

    @staticmethod
    def udp_socket(bind: str, port: int) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
        sock.bind((bind, port))
        sock.setblocking(False)
        return sock

    def accept(self, sock: socket.socket) -> None:
        client, _address = sock.accept()
        client.setblocking(False)
        self.clients.add(client)
        self.selector.register(client, selectors.EVENT_READ, self.echo)

    def echo(self, client: socket.socket) -> None:
        try:
            data = client.recv(65535)
            if not data:
                self.close_client(client)
                return
            client.sendall(data)
        except (BrokenPipeError, ConnectionError):
            self.close_client(client)

    def close_client(self, client: socket.socket) -> None:
        try:
            self.selector.unregister(client)
        except KeyError:
            pass
        self.clients.discard(client)
        client.close()

    def rx_burst(self, sock: socket.socket) -> None:
        payload, address = sock.recvfrom(65535)
        if len(payload) != 12 or payload[:8] != BURST_MAGIC:
            return
        count, payload_size = struct.unpack("!HH", payload[8:12])
        if not 1 <= count <= 64 or not 4 <= payload_size <= 1400:
            return
        for sequence in range(count):
            body = struct.pack("!HH", sequence, count)
            body += bytes([sequence & 0xFF]) * (payload_size - len(body))
            sock.sendto(body, address)

    def tx_stress(self, sock: socket.socket) -> None:
        payload, address = sock.recvfrom(65535)
        if len(payload) != 1200 or payload[:8] != TX_MAGIC:
            return
        sequence, count = struct.unpack("!II", payload[8:16])
        if not 1 <= count <= 4096 or sequence >= count:
            return
        if payload[16:] != bytes([sequence & 0xFF]) * (len(payload) - 16):
            return
        key = (address[0], address[1], count)
        if key in self.tx_completed:
            ack = TX_ACK_MAGIC + struct.pack("!II", count, count * len(payload))
            sock.sendto(ack, address)
            return
        received = self.tx_streams.setdefault(key, set())
        received.add(sequence)
        if len(received) != count:
            return
        print(
            f"LUNA_NET_PEER_TX_COMPLETE unique={len(received)} count={count}",
            flush=True,
        )
        del self.tx_streams[key]
        self.tx_completed.add(key)
        ack = TX_ACK_MAGIC + struct.pack("!II", count, count * len(payload))
        sock.sendto(ack, address)

    def run(self) -> None:
        while self.running:
            for key, _events in self.selector.select(timeout=0.25):
                key.data(key.fileobj)

    def close(self) -> None:
        for client in list(self.clients):
            self.close_client(client)
        for sock in (self.tcp, self.rx, self.tx):
            try:
                self.selector.unregister(sock)
            except KeyError:
                pass
            sock.close()
        self.selector.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--ready-file", type=Path)
    args = parser.parse_args()

    service = Service(args.bind)

    def stop(_signum: int, _frame: object) -> None:
        service.running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    if args.ready_file:
        args.ready_file.write_text("ready\n", encoding="ascii")
    try:
        service.run()
    finally:
        service.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
