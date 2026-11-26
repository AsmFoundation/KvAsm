#!/usr/bin/env python3
"""KvAsm from Python: put, get and delete a key, then upload a 1 MiB value in chunks under a long
key and download a byte range of it, through grpcio and stubs generated from
proto/kvasm/v1/kvasm.proto.

Run (Python 3.10 or later, which the pinned grpcio needs):
    pip install -r requirements.txt
    python kvasm_example.py [host:port]          (default localhost:50051)
A server to run it against:
    docker run --rm -p 50051:50051 ghcr.io/asmfoundation/kvasm:0.3.0

The stubs are generated on the first run, into gen/ next to this file: grpcio-tools carries its
own protoc, so nothing else needs installing. A newer kvasm.proto regenerates them.

Exit codes: 0 every call answered as the contract says; 1 one did not (each difference is
printed); 2 no server answered within 10 seconds.
"""

import hashlib
import os
import sys
from collections.abc import Callable, Iterator

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_ROOT = os.path.normpath(os.path.join(HERE, "..", "..", "proto"))
PROTO = os.path.join(PROTO_ROOT, "kvasm", "v1", "kvasm.proto")
GEN = os.path.join(HERE, "gen")


def generate_stubs() -> None:
    """Writes gen/kvasm/v1/kvasm_pb2.py and kvasm_pb2_grpc.py when they are missing or older
    than kvasm.proto.

    Raises:
        SystemExit: protoc refused the .proto (its message is on stderr).
    """
    stub = os.path.join(GEN, "kvasm", "v1", "kvasm_pb2_grpc.py")
    if os.path.exists(stub) and os.path.getmtime(stub) >= os.path.getmtime(PROTO):
        return
    os.makedirs(GEN, exist_ok=True)
    from grpc_tools import protoc  # imported here: only the first run needs grpcio-tools' protoc

    # -I is the proto root, so the generated module is kvasm.v1.kvasm_pb2, the package path
    # kvasm_pb2_grpc imports it by.
    code = protoc.main(["grpc_tools.protoc", f"-I{PROTO_ROOT}", f"--python_out={GEN}", f"--grpc_python_out={GEN}", PROTO])
    if code != 0:
        raise SystemExit(f"protoc failed on {PROTO} (exit {code})")


def wire_key(key: bytes) -> bytes:
    """The bytes to send in a request's key field for key: keys of 64 bytes or less unchanged,
    longer ones as the 65 bytes 0xFF followed by their SHA-512 (docs/PROTOCOL.md, "Long keys").

    Every client of one server must send long keys this way, as the .NET client does by default,
    or one logical key is stored under two different keys. Raw and compacted keys never collide:
    they never have the same length.

    Args:
        key: The application's key. An empty one passes unchanged, and the server refuses it
            with INVALID_ARGUMENT.

    Returns:
        The key as the server stores it.
    """
    return b"\xff" + hashlib.sha512(key).digest() if len(key) > 64 else key


def _crc32c_table() -> list[int]:
    """Builds the byte-at-a-time lookup table for crc32c, once at import (about a millisecond),
    so each byte of a checksum costs one lookup instead of eight shift-and-xor steps.

    Returns:
        256 entries: entry n is the CRC-32C register after shifting byte n through it with the
        reflected Castagnoli polynomial 0x82F63B78.
    """
    table = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


_TABLE = _crc32c_table()


def crc32c(data: bytes) -> int:
    """CRC-32C of data with the standard pre- and post-inversion, the checksum KvAsm stores
    (Python's zlib.crc32 is the other CRC-32, which the server rejects). Pure Python: about
    0.1 s per MiB, which is fine here; the crc32c package on PyPI does it in hardware.

    Args:
        data: The bytes.

    Returns:
        The checksum, 0..2^32-1; crc32c(b"123456789") is 0xE3069283.
    """
    crc = 0xFFFFFFFF
    for byte in data:
        crc = _TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def main(argv: list[str]) -> int:
    """Runs every call once and checks each answer against the contract in kvasm.proto.

    Args:
        argv: The command line; argv[1], when present, is the server's host:port.

    Returns:
        The exit code (see the module's docstring).

    Raises:
        AssertionError: The CRC-32C table is wrong, which would make every checksum wrong too.
        SystemExit: The stubs could not be generated.
    """
    target = argv[1] if len(argv) > 1 else "localhost:50051"
    assert crc32c(b"123456789") == 0xE3069283, "the CRC-32C table is wrong"
    generate_stubs()
    sys.path.insert(0, GEN)
    import grpc
    from kvasm.v1 import kvasm_pb2 as pb
    from kvasm.v1 import kvasm_pb2_grpc as rpc

    failures: list[str] = []

    def check(what: str, got: object, want: object) -> None:
        """Prints one answer, and records it in failures when it differs from the contract, so
        the run goes on and reports every difference, not just the first.

        Args:
            what: The call and the field, as printed.
            got: What the server answered (bytes over 16 long print as their length).
            want: What the contract says it must be.
        """
        shown = f"{len(got)} bytes" if isinstance(got, bytes) and len(got) > 16 else repr(got)
        print(f"{what}: {shown}")
        if got != want:
            failures.append(f"{what}: got {got!r}, expected {want!r}")

    with grpc.insecure_channel(target) as channel:
        try:
            grpc.channel_ready_future(channel).result(timeout=10)
        except grpc.FutureTimeoutError:
            print(f"No KvAsm server answers at {target}", file=sys.stderr)
            return 2
        try:
            run_calls(rpc.KvAsmStub(channel), pb, check)
        except grpc.RpcError as error:
            # A call the server refused (a CRC-32C it didn't match, a key over the limit) or a
            # connection that dropped: the status says which, instead of a traceback.
            failures.append(f"{error.code().name}: {error.details()}")

    for failure in failures:
        print("FAILED " + failure, file=sys.stderr)
    return 1 if failures else 0


def run_calls(kv, pb, check: Callable[[str, object, object], None]) -> None:
    """Makes every call once, in the order the module's docstring gives, passing each answer to
    check.

    Args:
        kv: The KvAsmStub on a ready channel.
        pb: The generated kvasm_pb2 module (the message classes).
        check: check(what, got, want) prints one answer and records a difference.

    Raises:
        grpc.RpcError: A call failed with a non-OK status; the calls after it are not made, so the
            keys it wrote may stay on the server until the next run deletes them.
    """
    # Unary calls. Delete first, so the run starts from a missing key whatever ran before.
    key = wire_key(b"example:python")
    kv.Delete(pb.DeleteRequest(key=key))
    check("Put created", kv.Put(pb.PutRequest(key=key, value=b"Ada")).created, True)
    got = kv.Get(pb.GetRequest(key=key))
    check("Get found", got.found, True)
    check("Get value", got.value, b"Ada")
    check("Delete deleted", kv.Delete(pb.DeleteRequest(key=key)).deleted, True)
    check("Get after Delete found", kv.Get(pb.GetRequest(key=key)).found, False)

    # Upload under a 70-byte key, which wire_key compacts to 65 bytes. The first message carries
    # the key and total_size, the next ones the data (64 KiB each), and the last the CRC-32C,
    # which the server checks before storing anything.
    big_key = wire_key(b"example:python:media/2026/11/26/a-file-name-long-enough-to-compact.bin")
    check("Long key sent as", len(big_key), 65)
    value = bytes((i * 31) % 251 for i in range(1 << 20))
    value_crc = crc32c(value)

    def upload_requests() -> Iterator[object]:
        """The Upload stream, in the order the contract requires.

        Returns:
            An iterator grpcio drains as it sends: the header, sixteen 64 KiB chunks, the
            checksum.
        """
        yield pb.UploadRequest(key=big_key, total_size=len(value))
        for offset in range(0, len(value), 64 * 1024):
            yield pb.UploadRequest(data=value[offset:offset + 64 * 1024])
        yield pb.UploadRequest(crc32c=value_crc)

    check("Upload crc32c", kv.Upload(upload_requests()).crc32c, value_crc)

    # Download bytes 1000..1999 in 256-byte chunks: the first message carries the whole value's
    # size, the last the CRC-32C of exactly the range. A client checks the bytes it received
    # against that checksum (a stream that ends without one is not a value); the example also
    # checks them against the bytes it uploaded.
    parts: list[bytes] = []
    total_size = range_crc = None
    for message in kv.Download(pb.DownloadRequest(key=big_key, offset=1000, length=1000, chunk_size=256)):
        if message.HasField("total_size"):
            total_size = message.total_size
        parts.append(message.data)
        if message.HasField("crc32c"):
            range_crc = message.crc32c
    received = b"".join(parts)
    check("Download crc32c", range_crc, crc32c(received))
    check("Download bytes", received, value[1000:2000])
    check("Download total_size", total_size, len(value))
    check("Delete big deleted", kv.Delete(pb.DeleteRequest(key=big_key)).deleted, True)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
