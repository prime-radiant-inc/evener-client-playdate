#!/usr/bin/env python3
"""One synthetic LAN TCP connection; no TLS, credentials, or external services."""

import argparse
import json
import select
import socket
import struct
import time

TOTAL_BYTES = 1024 * 1024
CHUNK = bytes([0x3C]) * (16 * 1024)
RECEIPT_BYTE = 0xA5
RECEIPT_BYTES = 117
LIMIT_SECONDS = 120


def report(event, **fields):
    print(json.dumps({"event": event, **fields}, sort_keys=True), flush=True)


def connection_info(connection):
    info = connection.getsockopt(socket.IPPROTO_TCP, 0x106, 104)
    return {"peer_window": struct.unpack_from("=I", info, 28)[0],
            "send_buffer_bytes": struct.unpack_from("=I", info, 32)[0]}


def serve(connection, peer, chunk_bytes, interval, expected_receipts):
    connection.setblocking(False)
    started = time.monotonic()
    next_send = started
    chunk = CHUNK[:chunk_bytes]
    next_report = started
    sent = received = send_calls = 0
    last_send = last_receive = started
    bad_bytes = 0
    outcome = "timeout"
    report("connected", peer=peer, chunk_bytes=len(chunk), interval_ms=interval * 1000, expected_tx=TOTAL_BYTES,
           receipt_bytes=RECEIPT_BYTES, expected_receipts=expected_receipts, limit_seconds=LIMIT_SECONDS)
    while time.monotonic() - started < LIMIT_SECONDS:
        before_select = time.monotonic()
        ready_to_send = sent < TOTAL_BYTES and before_select >= next_send
        timeout = (
            min(0.1, max(0.001, next_send - before_select))
            if sent < TOTAL_BYTES and not ready_to_send
            else 0.1
        )
        readable, writable, _ = select.select(
            [connection], [connection] if ready_to_send else [], [], timeout
        )
        now = time.monotonic()
        if readable:
            try:
                data = connection.recv(64 * 1024)
            except BlockingIOError:
                data = None
            except OSError as error:
                outcome = f"read_error:{error.errno}"
                break
            if data == b"":
                outcome = "eof"
                break
            if data:
                received += len(data)
                bad_bytes += sum(byte != RECEIPT_BYTE for byte in data)
                last_receive = now
                if received > expected_receipts * RECEIPT_BYTES:
                    outcome = "receipt_limit"
                    break
        if writable:
            try:
                count = connection.send(memoryview(chunk)[:min(len(chunk), TOTAL_BYTES - sent)])
            except BlockingIOError:
                count = 0
            except OSError as error:
                outcome = f"write_error:{error.errno}"
                break
            if count:
                sent += count
                send_calls += 1
                last_send = now
                next_send = now + interval
        if now >= next_report:
            report("progress", seconds=round(now - started, 3), tx=sent, rx=received,
                   receipts=received // RECEIPT_BYTES, receipt_remainder=received % RECEIPT_BYTES,
                   bad_bytes=bad_bytes, send_calls=send_calls,
                   tx_idle=round(now - last_send, 3), rx_idle=round(now - last_receive, 3),
                   **connection_info(connection))
            next_report = now + 1
    passed = (outcome == "eof" and sent == TOTAL_BYTES
              and received == expected_receipts * RECEIPT_BYTES and bad_bytes == 0)
    report("PASS" if passed else "FAIL", outcome=outcome, seconds=round(time.monotonic() - started, 3),
           tx=sent, rx=received, receipts=received // RECEIPT_BYTES,
           receipt_remainder=received % RECEIPT_BYTES, bad_bytes=bad_bytes, send_calls=send_calls)
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1", help="LAN address reachable from the Playdate")
    parser.add_argument("--port", type=int, default=19184, help="nonprivileged port; must match main.c")
    parser.add_argument("--receipts", type=int, choices=(1, 10), default=10,
                        help="expected receipt count: 1 for download, 10 for duplex/fence")
    parser.add_argument("--chunk", type=int, default=16384)
    parser.add_argument("--interval-ms", type=float, default=0)
    args = parser.parse_args()
    if not 1 <= args.chunk <= 16384 or args.interval_ms < 0:
        parser.error("invalid pacing parameters")
    if not 1024 <= args.port <= 65535:
        parser.error("port must be between 1024 and 65535")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.bind, args.port))
        listener.listen(1)
        listener.settimeout(LIMIT_SECONDS)
        report("listening", bind=args.bind, port=args.port, accept_limit_seconds=LIMIT_SECONDS)
        try:
            connection, peer = listener.accept()
        except TimeoutError:
            report("FAIL", outcome="no_connection")
            return 1
        with connection:
            return 0 if serve(connection, peer, args.chunk, args.interval_ms / 1000, args.receipts) else 1


if __name__ == "__main__":
    raise SystemExit(main())
