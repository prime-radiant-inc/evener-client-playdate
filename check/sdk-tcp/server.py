#!/usr/bin/env python3
"""One synthetic LAN TCP connection; no TLS, credentials, or external services."""

import argparse
from contextlib import ExitStack
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
LOCKSTEP_BYTES = 1200


def report(event, **fields):
    print(json.dumps({"event": event, **fields}, sort_keys=True), flush=True)


def connection_info(connection):
    info = connection.getsockopt(socket.IPPROTO_TCP, 0x106, 104)
    return {"peer_window": struct.unpack_from("=I", info, 28)[0],
            "send_buffer_bytes": struct.unpack_from("=I", info, 32)[0]}


def serve(connection, peer, chunk_bytes, interval, expected_receipts, receipt_connection=None, lockstep=False):
    receipt_connection = receipt_connection or connection
    connection.setblocking(False)
    receipt_connection.setblocking(False)
    read_connections = {connection, receipt_connection}
    started = time.monotonic()
    next_send = started
    chunk = CHUNK[:chunk_bytes]
    next_report = started
    sent = received = send_calls = 0
    last_send = last_receive = started
    bad_bytes = 0
    outcome = "timeout"
    limit_seconds = 180 if lockstep else LIMIT_SECONDS
    report("connected", peer=peer, chunk_bytes=len(chunk), interval_ms=interval * 1000, expected_tx=TOTAL_BYTES,
           receipt_bytes=RECEIPT_BYTES, expected_receipts=expected_receipts, limit_seconds=limit_seconds, lockstep=lockstep)
    while time.monotonic() - started < limit_seconds:
        before_select = time.monotonic()
        send_limit = min(TOTAL_BYTES, (received // RECEIPT_BYTES + 1) * LOCKSTEP_BYTES) if lockstep else TOTAL_BYTES
        ready_to_send = connection in read_connections and sent < send_limit and before_select >= next_send
        timeout = (
            min(0.1, max(0.001, next_send - before_select))
            if connection in read_connections and sent < send_limit and not ready_to_send
            else 0.1
        )
        readable, writable, _ = select.select(
            list(read_connections), [connection] if ready_to_send else [], [], timeout
        )
        now = time.monotonic()
        for ready in readable:
            try:
                data = ready.recv(64 * 1024)
            except BlockingIOError:
                data = None
            except OSError as error:
                outcome = f"read_error:{error.errno}"
                break
            if data == b"":
                read_connections.remove(ready)
            if data:
                if ready is not receipt_connection:
                    outcome = "unexpected_input"
                    break
                received += len(data)
                bad_bytes += sum(byte != RECEIPT_BYTE for byte in data)
                last_receive = now
                if received > expected_receipts * RECEIPT_BYTES:
                    outcome = "receipt_limit"
                    break
        if outcome != "timeout":
            break
        if not read_connections:
            outcome = "eof"
            break
        if writable:
            try:
                count = connection.send(memoryview(chunk)[:min(len(chunk), send_limit - sent)])
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
    parser.add_argument("--receipts", type=int, choices=(1, 10), nargs="+", default=[10],
                        help="receipt counts for sequential runs: 1 for download, 10 for duplex/fence/split")
    parser.add_argument("--lockstep", action="store_true", help="wait for a receipt after every 1200-byte block")
    parser.add_argument("--split", action="store_true", help="accept receipts separately on port + 1")
    parser.add_argument("--block-first", action="store_true", help="hold an unread connection before the measured transfer")
    parser.add_argument("--chunk", type=int, default=16384)
    parser.add_argument("--interval-ms", type=float, default=0)
    args = parser.parse_args()
    if args.lockstep:
        if args.split or args.block_first or args.receipts != [10]:
            parser.error("lockstep is a separate transfer experiment")
        args.receipts = [(TOTAL_BYTES + LOCKSTEP_BYTES - 1) // LOCKSTEP_BYTES]
    if args.split and args.block_first:
        parser.error("split and blocked-write experiments are separate")
    if not 1 <= args.chunk <= 16384 or args.interval_ms < 0:
        parser.error("invalid pacing parameters")
    if not 1024 <= args.port <= (65534 if args.split else 65535):
        parser.error("port must be between 1024 and 65535")
    with ExitStack() as resources:
        listeners = []
        for port in ([args.port, args.port + 1] if args.split else [args.port]):
            listener = resources.enter_context(socket.socket(socket.AF_INET, socket.SOCK_STREAM))
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if args.block_first:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            listener.bind((args.bind, port))
            listener.listen(1)
            listener.settimeout(LIMIT_SECONDS)
            listeners.append(listener)
            report("listening", bind=args.bind, port=port, accept_limit_seconds=LIMIT_SECONDS)
        if args.block_first:
            try:
                blocked, peer = listeners[0].accept()
            except TimeoutError:
                report("FAIL", outcome="no_pressure_connection")
                return 1
            resources.enter_context(blocked)
            report("blocking_receipts", peer=peer, receive_buffer=blocked.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF))
        results = []
        for expected_receipts in args.receipts:
            with ExitStack() as clients:
                receipt_connection = None
                try:
                    if args.split:
                        receipt_connection, _ = listeners[1].accept()
                        clients.enter_context(receipt_connection)
                    connection, peer = listeners[0].accept()
                    clients.enter_context(connection)
                except TimeoutError:
                    report("FAIL", outcome="no_connection")
                    return 1
                results.append(serve(connection, peer, args.chunk, args.interval_ms / 1000,
                                     expected_receipts, receipt_connection, args.lockstep))
        return 0 if all(results) else 1



if __name__ == "__main__":
    raise SystemExit(main())
