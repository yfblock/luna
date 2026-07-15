#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Minimal external Ethernet peer for the Phase 2.5 QEMU socket backend."""

import argparse
import signal
import socket
import struct
from pathlib import Path


GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("525400123401")
GUEST_IP = socket.inet_aton("10.0.2.15")
PEER_IP = socket.inet_aton("10.0.2.2")
TCP_PORT = 18080
UDP_PORT = 18081
TX_UDP_PORT = 18082
BURST_MAGIC = b"LUNABRST"
TX_MAGIC = b"LUNATX25"
TX_ACK_MAGIC = b"LUNATXOK"


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ethernet(payload: bytes, ethertype: int) -> bytes:
    return GUEST_MAC + PEER_MAC + struct.pack("!H", ethertype) + payload


def ipv4(payload: bytes, protocol: int, ident: int = 0x2525) -> bytes:
    header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        20 + len(payload),
        ident,
        0x4000,
        64,
        protocol,
        0,
        PEER_IP,
        GUEST_IP,
    )
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    return ethernet(header + payload, 0x0800)


def tcp_segment(src_port: int, dst_port: int, seq: int, ack: int,
                flags: int, payload: bytes = b"") -> bytes:
    header = struct.pack(
        "!HHIIBBHHH",
        src_port,
        dst_port,
        seq & 0xFFFFFFFF,
        ack & 0xFFFFFFFF,
        5 << 4,
        flags,
        65535,
        0,
        0,
    )
    pseudo = PEER_IP + GUEST_IP + struct.pack("!BBH", 0, 6,
                                               len(header) + len(payload))
    value = checksum(pseudo + header + payload)
    header = header[:16] + struct.pack("!H", value) + header[18:]
    return ipv4(header + payload, 6)


def udp_segment(src_port: int, dst_port: int, payload: bytes,
                ident: int) -> bytes:
    header = struct.pack("!HHHH", src_port, dst_port, 8 + len(payload), 0)
    return ipv4(header + payload, 17, ident)


class Peer:
    def __init__(self) -> None:
        self.connections: dict[int, dict[str, int]] = {}
        self.tx_streams: dict[tuple[int, int], set[int]] = {}
        self.tx_completed: set[tuple[int, int]] = set()

    def handle_arp(self, packet: bytes) -> bytes | None:
        if len(packet) < 42:
            return None
        arp = packet[14:42]
        hardware, protocol, hlen, plen, operation = struct.unpack(
            "!HHBBH", arp[:8]
        )
        sender_mac = arp[8:14]
        sender_ip = arp[14:18]
        target_ip = arp[24:28]
        if (hardware, protocol, hlen, plen, operation) != (1, 0x0800, 6, 4, 1):
            return None
        if target_ip != PEER_IP or sender_ip != GUEST_IP:
            return None
        reply = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2)
        reply += PEER_MAC + PEER_IP + sender_mac + sender_ip
        return ethernet(reply, 0x0806)

    def handle_icmp(self, ip_payload: bytes) -> bytes | None:
        if len(ip_payload) < 8 or ip_payload[0] != 8 or ip_payload[1] != 0:
            return None
        reply = bytes([0, 0, 0, 0]) + ip_payload[4:]
        reply = reply[:2] + struct.pack("!H", checksum(reply)) + reply[4:]
        return ipv4(reply, 1)

    def handle_tcp(self, ip_payload: bytes) -> bytes | None:
        if len(ip_payload) < 20:
            return None
        src_port, dst_port, seq, ack = struct.unpack("!HHII", ip_payload[:12])
        offset = (ip_payload[12] >> 4) * 4
        flags = ip_payload[13]
        if dst_port != TCP_PORT or offset < 20 or offset > len(ip_payload):
            return None
        payload = ip_payload[offset:]
        state = self.connections.get(src_port)
        if flags & 0x02:  # SYN
            server_seq = 0x25250000 | src_port
            self.connections[src_port] = {
                "client_next": (seq + 1) & 0xFFFFFFFF,
                "server_next": (server_seq + 1) & 0xFFFFFFFF,
            }
            return tcp_segment(TCP_PORT, src_port, server_seq, seq + 1, 0x12)
        if state is None:
            return tcp_segment(TCP_PORT, src_port, 0, seq + len(payload), 0x04)
        consumed = len(payload) + (1 if flags & 0x01 else 0)
        if consumed:
            state["client_next"] = (seq + consumed) & 0xFFFFFFFF
        if payload:
            response = tcp_segment(
                TCP_PORT,
                src_port,
                state["server_next"],
                state["client_next"],
                0x18,
                payload,
            )
            state["server_next"] = (
                state["server_next"] + len(payload)
            ) & 0xFFFFFFFF
            return response
        if flags & 0x01:
            return tcp_segment(
                TCP_PORT, src_port, state["server_next"],
                state["client_next"], 0x10
            )
        return None

    def handle_tx_stream(self, src_port: int,
                         payload: bytes) -> bytes | None:
        if len(payload) != 1200 or payload[:8] != TX_MAGIC:
            return None
        sequence, count = struct.unpack("!II", payload[8:16])
        if not 1 <= count <= 4096 or sequence >= count:
            return None
        if payload[16:] != bytes([sequence & 0xFF]) * (len(payload) - 16):
            return None
        key = (src_port, count)
        if key in self.tx_completed:
            ack = TX_ACK_MAGIC + struct.pack("!II", count,
                                             count * len(payload))
            return udp_segment(TX_UDP_PORT, src_port, ack, 0x4000)
        received = self.tx_streams.setdefault(key, set())
        received.add(sequence)
        if len(received) == count:
            print(
                f"LUNA_NET_PEER_TX_COMPLETE unique={len(received)} "
                f"count={count}",
                flush=True,
            )
        if len(received) != count:
            return None
        del self.tx_streams[key]
        self.tx_completed.add(key)
        ack = TX_ACK_MAGIC + struct.pack("!II", count, count * len(payload))
        return udp_segment(TX_UDP_PORT, src_port, ack, 0x4000)

    def handle_udp(self, ip_payload: bytes) -> list[bytes] | bytes | None:
        if len(ip_payload) < 8:
            return None
        src_port, dst_port, length, _value = struct.unpack("!HHHH",
                                                           ip_payload[:8])
        if length < 8 or length > len(ip_payload):
            return None
        payload = ip_payload[8:length]
        if dst_port == TX_UDP_PORT:
            return self.handle_tx_stream(src_port, payload)
        if dst_port != UDP_PORT:
            return None
        if len(payload) != 12 or payload[:8] != BURST_MAGIC:
            return None
        count, payload_size = struct.unpack("!HH", payload[8:12])
        if not 1 <= count <= 64 or not 4 <= payload_size <= 1400:
            return None
        replies = []
        for sequence in range(count):
            body = struct.pack("!HH", sequence, count)
            body += bytes([sequence & 0xFF]) * (payload_size - len(body))
            replies.append(udp_segment(UDP_PORT, src_port, body,
                                       0x3000 + sequence))
        return replies

    def handle_ipv4(self, packet: bytes) -> bytes | list[bytes] | None:
        if len(packet) < 34:
            return None
        ip = packet[14:]
        header_length = (ip[0] & 0x0F) * 4
        total_length = struct.unpack("!H", ip[2:4])[0]
        if (ip[0] >> 4 != 4 or header_length < 20 or
                total_length > len(ip) or ip[16:20] != PEER_IP or
                ip[12:16] != GUEST_IP):
            return None
        payload = ip[header_length:total_length]
        if ip[9] == 1:
            return self.handle_icmp(payload)
        if ip[9] == 6:
            return self.handle_tcp(payload)
        if ip[9] == 17:
            return self.handle_udp(payload)
        return None

    def handle(self, packet: bytes) -> list[bytes]:
        if len(packet) < 14 or packet[6:12] != GUEST_MAC:
            return []
        ethertype = struct.unpack("!H", packet[12:14])[0]
        reply: bytes | list[bytes] | None = None
        if ethertype == 0x0806:
            reply = self.handle_arp(packet)
        elif ethertype == 0x0800:
            reply = self.handle_ipv4(packet)
        if reply is None:
            return []
        if isinstance(reply, bytes):
            return [reply]
        return reply


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-port", type=int, default=18081)
    parser.add_argument("--qemu-port", type=int, default=18082)
    parser.add_argument("--ready-file", type=Path)
    args = parser.parse_args()

    running = True

    def stop(_signum: int, _frame: object) -> None:
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    sock.bind(("127.0.0.1", args.listen_port))
    sock.settimeout(0.25)
    if args.ready_file:
        args.ready_file.write_text("ready\n", encoding="ascii")
    peer = Peer()
    while running:
        try:
            packet, _address = sock.recvfrom(65535)
        except TimeoutError:
            continue
        for reply in peer.handle(packet):
            sock.sendto(reply, ("127.0.0.1", args.qemu_port))
    sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
