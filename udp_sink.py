#!/usr/bin/env python3
"""UDP receiver that demonstrates and measures rawsendmpeg2ts.

Bind a UDP port, capture datagrams for a fixed window, and report the datagram
count, mean throughput, and inter-arrival jitter percentiles. Optionally save
the first N received bytes so they can be diffed against the source .ts to prove
the pump does not mutate payload.

Loopback arrival timing is only an indicative proxy for pacing: it is enough to
show the pump is uniform and burst-free, but the authoritative on-wire check is a
PCAP captured at the real receiver (see README).
"""
import argparse
import json
import socket
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    ap.add_argument("--port", type=int, required=True, help="bind port")
    ap.add_argument("--seconds", type=float, default=8.0, help="capture window (default 8)")
    ap.add_argument("--save", metavar="FILE", help="write the first --save-bytes received bytes here")
    ap.add_argument("--save-bytes", type=int, default=1_000_000, help="bytes to save for a fidelity diff")
    ap.add_argument("--stats", metavar="FILE", help="also write the JSON stats to this file")
    ap.add_argument("--ready", metavar="FILE", help="create this file once bound (test synchronization)")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    s.bind((args.host, args.port))
    if args.ready:
        with open(args.ready, "w") as fp:
            fp.write("1")

    deadline = time.monotonic() + args.seconds
    times = []
    total = 0
    cap = bytearray()
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        s.settimeout(remaining)
        try:
            data = s.recv(4096)
        except socket.timeout:
            break
        times.append(time.monotonic_ns())
        total += len(data)
        if args.save and len(cap) < args.save_bytes:
            cap.extend(data[: args.save_bytes - len(cap)])

    res = {"datagrams": len(times), "total_bytes": total}
    if len(times) >= 2:
        span = times[-1] - times[0]
        res["span_s"] = round(span / 1e9, 4)
        res["throughput_mbps"] = round((total * 8) / (span / 1e9) / 1e6, 4) if span > 0 else None
        iv = sorted((times[i + 1] - times[i]) / 1e6 for i in range(len(times) - 1))
        n = len(iv)
        res["interval_ms_min"] = round(iv[0], 4)
        res["interval_ms_p50"] = round(iv[n // 2], 4)
        res["interval_ms_p99"] = round(iv[min(n - 1, int(n * 0.99))], 4)
        res["interval_ms_max"] = round(iv[-1], 4)
        res["interval_ms_mean"] = round(sum(iv) / n, 4)

    if args.save:
        with open(args.save, "wb") as fp:
            fp.write(cap)
    if args.stats:
        with open(args.stats, "w") as fp:
            json.dump(res, fp, indent=2)
    print(json.dumps(res, indent=2))


if __name__ == "__main__":
    main()
