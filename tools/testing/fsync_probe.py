#!/usr/bin/env python3
"""fsync latency probe (WO-065, SL-EP-080). Stdlib only, no subprocess, no network (ADR-0004).

Run with `python3 -I tools/testing/fsync_probe.py DIR [--files N] [--bytes S]`.

Writes N files of S bytes (defaults 5 files of 4 MiB) under DIR, then times os.fsync on each file and on DIR with
time.perf_counter_ns. Prints one JSON line per sample and one summary line, removes the files, and always exits 0
(it measures, it does not gate):
  {"file": "probe-0.bin", "bytes": 4194304, "fsync_ms": 1.234, "dir_fsync_ms": 0.456}
  {"summary": {"samples": 5, "min_ms": 1.0, "median_ms": 1.2, "max_ms": 3.4}}
The summary statistics are over the file fsync times.
"""

import json
import os
import statistics
import sys
import time

DEFAULT_FILES = 5
DEFAULT_BYTES = 4 * 1024 * 1024
PREFIX = "fsync-probe-"


def _fsync_ms(fd):
    start = time.perf_counter_ns()
    os.fsync(fd)
    return (time.perf_counter_ns() - start) / 1e6


def probe(directory, files=DEFAULT_FILES, size=DEFAULT_BYTES):
    """Return (samples, summary); the probe files are removed before returning."""
    payload = b"\xa5" * size
    paths = []
    for index in range(files):
        path = os.path.join(directory, "%s%d.bin" % (PREFIX, index))
        with open(path, "wb") as handle:
            handle.write(payload)
        paths.append(path)
    samples = []
    try:
        dir_fd = os.open(directory, os.O_RDONLY)
        try:
            for path in paths:
                fd = os.open(path, os.O_RDONLY)
                try:
                    file_ms = _fsync_ms(fd)
                finally:
                    os.close(fd)
                samples.append(
                    {
                        "file": os.path.basename(path),
                        "bytes": size,
                        "fsync_ms": round(file_ms, 3),
                        "dir_fsync_ms": round(_fsync_ms(dir_fd), 3),
                    }
                )
        finally:
            os.close(dir_fd)
    finally:
        for path in paths:
            try:
                os.remove(path)
            except OSError:
                pass
    times = [sample["fsync_ms"] for sample in samples]
    summary = {
        "samples": len(times),
        "min_ms": min(times) if times else 0.0,
        "median_ms": round(statistics.median(times), 3) if times else 0.0,
        "max_ms": max(times) if times else 0.0,
    }
    return samples, summary


def main(argv):
    files, size, directory = DEFAULT_FILES, DEFAULT_BYTES, None
    args = list(argv)
    try:
        while args:
            arg = args.pop(0)
            if arg == "--files":
                files = int(args.pop(0))
            elif arg == "--bytes":
                size = int(args.pop(0))
            elif directory is None:
                directory = arg
            else:
                raise ValueError(arg)
        if directory is None or files < 1 or size < 1:
            raise ValueError("arguments")
    except (IndexError, ValueError):
        print("usage: fsync_probe.py DIR [--files N] [--bytes S]", file=sys.stderr)
        return 0
    try:
        samples, summary = probe(directory, files, size)
    except OSError as err:
        print("fsync_probe: %s" % err, file=sys.stderr)
        return 0
    for sample in samples:
        print(json.dumps(sample))
    print(json.dumps({"summary": summary}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
