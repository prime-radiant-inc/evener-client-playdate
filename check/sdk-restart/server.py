#!/usr/bin/env python3
"""Hold synthetic TCP connections open; record accepts and runtime disconnects."""
import argparse
import json
import math
import selectors
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--seconds", type=float, default=60)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("seconds must be positive and finite")
    started = time.monotonic()
    def report(event, **fields):
        print(json.dumps({"event": event, "seconds": round(time.monotonic() - started, 3), **fields}), flush=True)
    with selectors.DefaultSelector() as selector, socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.bind, 19186))
        listener.listen(8)
        listener.setblocking(False)
        selector.register(listener, selectors.EVENT_READ)
        report("listening")
        accepted = 0
        try:
            while True:
                remaining = started + args.seconds - time.monotonic()
                if remaining <= 0:
                    break
                for key, _ in selector.select(remaining):
                    if key.fileobj is listener:
                        connection, _ = listener.accept()
                        connection.setblocking(False)
                        accepted += 1
                        selector.register(connection, selectors.EVENT_READ, accepted)
                        report("accepted", connection=accepted)
                    else:
                        try:
                            data = key.fileobj.recv(1024)
                        except BlockingIOError:
                            continue
                        except OSError:
                            data = b""
                        if data:
                            report("unexpected_data", connection=key.data, bytes=len(data))
                        else:
                            report("disconnected", connection=key.data)
                            selector.unregister(key.fileobj)
                            key.fileobj.close()
        finally:
            for key in list(selector.get_map().values()):
                if key.fileobj is not listener:
                    key.fileobj.close()
        report("finished", accepted=accepted)


if __name__ == "__main__":
    main()
