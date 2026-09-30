# 2026-09-30 — Upload/Download: measured, profiled, optimized

The streaming calls of [ADR 0010](../../../docs/adr/0010-streaming-upload-download.md)
(client-streaming Upload, server-streaming Download) measured end to end,
profiled on the server and on the .NET client, and optimized one change
at a time, each change proven by a paired A/B before it was kept.

## Tools built for it

| Tool | What |
|---|---|
| `kvbench --upload / --download` ([benchmarks/kvbench](../../kvbench)) | streaming workloads: every request moves one whole value of `--value-size` bytes in `--chunk`-byte messages; uploads generated straight into the output buffer (nghttp2 `NO_COPY`), downloads parsed and compared with the expected bytes in place; the server's CRC-32C checked against an O(1) CRC combination per key. `--server-pid` reads the server's CPU from its threads' `schedstat` (ns-exact) |
| `abstream.py` | concurrent paired A/B of two server binaries: both run AT THE SAME TIME on pinned P-core vCPUs 2 and 6, clients on E-core sets 16-23 / 24-31, slots swapped every block; value MB/s, server CPU ns per byte, p50/p99; also unary GET configs as a guard for the small-request path |
| `client/dotnet/bench/KvNode.Client.Bench` | the .NET client's Upload/Download throughput (and unary Get, `--mode get`, as the guard), client CPU, bytes allocated per transfer, GC counts; `--window` / `--connections` set the library's options, `--clients N` runs N separate clients; published self-contained for linux-x64 so it runs next to the pinned server inside the VM |
| `abdotnet.py` / `dnseq.py` | .NET client A/Bs: concurrent pairs (as abstream), and sequential ABBA rounds with one client alone on all E-cores, which the noisy .NET runs needed; `dnseq.py` also places the server (`SERVER_CPUS`, `WORKERS`) |

## Profiles (one server worker, perf cpu-clock by PID, kernel symbols)

Baseline, upload of 1 MiB values over 4 connections, 6.4 GB/s:

| Share | Symbol | |
|---|---|---|
| 48.8% | `kv_crc32c_copy_hw` | the fused CRC-32C + copy into the record: ONE chain of `crc32` (3-cycle latency), ~13 GB/s |
| 18.1% | `copy_user_enhanced_fast_string` | the kernel copy of `read()` |
| 9.7% | `__wake_up_common_lock` | waking the co-located client (loopback artifact) |
| 5.6% | `__memmove_avx_unaligned_erms` | the partial-frame carry of 64 KiB reads |

Download of 1 MiB values, 4.65 GB/s: `kv_crc32c_copy_hw` 33.5%, client
wakeups 29.6%, the kernel send copy 14.1%.

## Server optimizations (each a concurrent A/B, 6 blocks, sign test)

Sign test: 6/6 blocks is p = 0.031, the smallest a 6-block test can give.

| Change | Result | Files |
|---|---|---|
| **Folding CRC-32C** (VPCLMULQDQ/PCLMULQDQ, 8 lanes; `crc32` chain and tables kept) | uploads **+7.2% / +12.6% / +12.1%** (64 KiB / 1 MiB / 16 MiB values), downloads **+23.0% / +25.3% / +22.0%**, single connection +8.5% / +22.9%; server CPU per byte −7..−21%; all 6/6 | `ab-crc.*` |
| **Non-temporal stores into large uploads' records** (≥ 64 KiB, line-aligned) | uploads **+13.3% / +31.3% / +24.9%**, single connection +16.3%, all 6/6; download control 0.997 (3/6) | `ab-nt.*` |
| **256 KiB reads** (`READ_CHUNK`, was 64 KiB) | uploads of 64 KiB values **1.88x** (server CPU/byte −47%, p50 −56%), 1 MiB **+13.4%**, 16 MiB +4.7%, 1 conn +6.7%, all 6/6; download 1.007, GET 0.995 (both 3/6, neutral) | `ab-readchunk.*`, `screens/` |

Read-size screens (2-4 blocks, `screens/`): against 64 KiB, 128 KiB gave
1.41x / 1.08x (64 KiB / 1 MiB uploads), 256 KiB 1.87x / 1.13x, 512 KiB
2.04x / 1.06x; against 256 KiB, 512 KiB and 1 MiB lost 7-17% on 1 MiB and
16 MiB uploads (their buffer no longer stays in L2 beside the socket data
being copied in). `screen-kvnode-rc512.jsonl` holds both screens of that
variant (the first 6 records against 64 KiB, the rest against 256 KiB).

CRC microbenchmarks (`server/tests/test_crc32c`, vCPU 2, MB/s):

| Case | memcpy | sse42 + copy | pclmul + copy | vpclmul + copy | vpclmul NT |
|---|---|---|---|---|---|
| 16 KiB, hot | 210,315 | 14,771 | 45,066 | 87,934 | 42,410 |
| 16 KiB hot → 64 MiB of records (upload) | 16,075-18,504 | 13,178 | 15,588 | 16,626 | **40,345-44,391** |
| 64 MiB of records → 16 KiB hot (download) | 26,947 | 11,934 | 18,736 | 18,786 | 16,747 |

**The whole effect — the original build (A) against the final one (B)**,
the same concurrent A/B on a quiet host (2.3 of 32 CPUs busy outside the
VM), 6 blocks, every row 6/6 (p = 0.031) except the GET guard
(`ab-final-server.*`):

| Config | A | B | B/A | Server CPU per byte | p50 per transfer |
|---|---|---|---|---|---|
| upload, 64 KiB values, 8 conns x 4 deep | 3.52 GB/s | **8.10 GB/s** | **2.30x** | −56.5% | −65.3% |
| upload, 1 MiB, 4 conns | 6.36 GB/s | **10.80 GB/s** | **1.70x** | −41.2% | −42.0% |
| upload, 16 MiB, 2 conns | 5.90 GB/s | **9.21 GB/s** | **1.56x** | −37.6% | −38.6% |
| download, 64 KiB, 8 x 4 | 4.15 GB/s | **5.20 GB/s** | **1.24x** | −19.1% | −20.2% |
| download, 1 MiB, 4 conns | 4.69 GB/s | **5.99 GB/s** | **1.28x** | −21.8% | −21.5% |
| download, 16 MiB, 2 conns | 5.53 GB/s | **6.75 GB/s** | **1.22x** | −17.7% | −18.0% |
| GET, 100 B, 64 x 16 (guard) | 2.49 M/s | 2.51 M/s | 1.000 (4/6) | 1.000 | 1.002 |

One server core over loopback, both variants running at once (so each
gets part of the memory bus: alone, B's 1 MiB uploads reach ~11 GB/s).

What the final server still spends on a download (perf by PID, 1 MiB
values over 4 connections, 6.6 GB/s on one worker): **36.4%** the fused
CRC + copy of each chunk from the record into the output buffer (the
`vpclmul` loop, a NASM macro-local label `..@76.loop` in the report),
**24.9%** `__wake_up_common_lock` (waking the co-located client; ADR 0009),
**13.5%** `copy_user_enhanced_fast_string` (the kernel's send copy). The
36% is the next lever, not taken here: a CRC stored with each record and
whole-value downloads sent straight from the pinned record (`sendmsg` with
an iovec into it) would drop both the copy and the CRC pass — ADR 0010
section 6 sketches it.

## .NET client

Baseline (original client, 4 concurrent 1 MiB transfers, one pinned
server worker): **upload 2.0 GB/s on 4.7 client cores, download 1.3 GB/s
on 3.6 cores, the server idle half the time**; a 1 MiB download allocated
1,078 KB (a `DownloadResponse` and a fresh copy of every 64 KiB chunk)
and ran 378 gen0 collections in 5 s. Profile: ~36% of the client's CPU in
cross-thread wakeups inside the HTTP/2 stack, ~13% libc copies, 40% of
samples on the thread pool's spin-wait when spinning is on.

| Change | Result | Files |
|---|---|---|
| **Folding CRC-32C in `Crc32C`** (Pclmulqdq[.V256], chain fallback) | client CPU per GB **−9% (1 MiB uploads, 6/6, p=0.031)**, −8% (64 KiB uploads, 6/6), downloads −9% / −11% (5/6, 4/6); throughput +12.8% upload (5/6) | `ab-dotnet-crc.*` (concurrent pairs) |
| **Zero-alloc streaming codec** (`StreamingCodec`: one reused Upload frame, pooled Download frames with ArrayPool data) | 1 MiB download: allocated **1,072 KB → 23 KB per transfer (−97.9%)**, gen0 111 → 2.6 per s, **throughput +14.9%, client CPU/GB −14.6%, p50 −12.2% (all 6/6, p=0.031)**; 16 MiB download: **17.0 MB → 152 KB (−99.1%)**; uploads: −1..−4% allocation (gRPC's own per-write state remains), timing neutral | `seq-dotnet-codec.*` (sequential ABBA, spinning off) |
| **Upload chunks 64 KiB → 1 MiB** (`UploadChunkBytes` default; screens, 4 rounds each) | 256 KiB vs 64 KiB: 1 MiB uploads **1.34x** (4/4), CPU/GB −24%, allocation −41%; 16 MiB uploads 1.22x (3/4). 1 MiB vs 256 KiB: 1 MiB uploads another **1.15x** (4/4), CPU/GB −11%, allocation −17%; 16 MiB 1.03x (3/4). Download chunks of 256 KiB were **slower** (0.91x, 0/4; gRPC reads a whole message before handing it over): the download default stays the server's 64 KiB | `screens/seq-dotnet-chunk-*` |
| **16 MiB HTTP/2 receive window** (`StreamReceiveWindowBytes`; .NET starts every stream at 64 KiB, returns credit every eighth of the window and only as data is read, and restarts its RTT-based growth per stream) | 1 MiB downloads **1.50x** (6/6, p=0.031), client CPU/GB −30%, allocation −24%, p50 −32%, p99 −49%; 16 MiB downloads **1.20x** (6/6), CPU/GB −20%, p50 −15%, server CPU −22%; uploads 1.00 / 1.01 (the window only paces what the client receives). Screens (3-4 rounds, one build with `--window`): 64 KiB→4 MiB 1.42x / 1.12x (4/4), 1→4 MiB 1.24x / 1.18x (3/3), 4→16 MiB 1.10x / 1.06x (3/3), 8→16 MiB 1.00x / 1.07x (1/3, 3/3) — the window should hold the whole transfer | `seq-dotnet-window.*` (the library change, 6 rounds), `screens/seq-dotnet-window-*` |
| **4 HTTP/2 connections per client** (`Connections`; calls take them in turn, lock-free). One connection runs every frame of every call through .NET's single reader and writer loop, and KvNode serves it from one worker thread for life | one server worker, 4 against 1 connection (6/6, p=0.031): 1 MiB uploads **2.82x** (2.54 → 7.29 GB/s, client CPU/GB −41%, p50 −66%), 16 MiB uploads **1.73x**, 1 MiB downloads **1.32x**, 16 MiB **1.41x** (the one server core saturated), Gets of 100 B with 64 in flight **1.38x** (client CPU per Get +5%), one Get at a time 1.005 (neutral). A 4-worker server, 8 concurrent 1 MiB transfers (3 rounds): 1→4 connections uploads 3.0 → 7.7 GB/s, downloads 4.1 → **11.4 GB/s**, Gets 1.22x (CPU per Get +27%); 4→8 uploads 1.26x, downloads 1.03x, Gets 1.12x (`screens/seq-dotnet-connections-w4-*`). The first screen used 4 separate clients (`--clients 4`, `screens/seq-dotnet-connections-1-4.*`): 2.90x / 1.34x | `seq-dotnet-connections.*` (the library pool, 6 rounds), `screens/seq-dotnet-connections-*` |

**The whole client effect — the original client (A) against the final one
(B)**, sequential ABBA rounds on a quiet host, one client alone on all
E-cores with spinning off, the same optimized server (one worker), 6
rounds, each build with its own defaults (A: 64 KiB upload chunks, .NET's
receive window, one connection; B: 1 MiB chunks, a 16 MiB window, 4
connections). Every cell 6/6, p = 0.031 (`seq-dotnet-final.*`):

| Config | Throughput | Client CPU per GB | Allocated per transfer | p50 | p99 |
|---|---|---|---|---|---|
| upload, 1 MiB, 4 concurrent | 1.83 → **7.23 GB/s (3.89x)** | −58.3% | 39.5 → 20.2 KB (−48.8%) | −74.4% | −73.4% |
| download, 1 MiB, 4 concurrent | 2.11 → **5.24 GB/s (2.47x)** | −49.0% | 1,073 → 17.6 KB (−98.4%) | −56.9% | −69.0% |
| upload, 16 MiB, 2 concurrent | 1.60 → **4.17 GB/s (2.59x)** | −50.9% | 525 → 196 KB (−62.5%) | −61.3% | −62.9% |
| download, 16 MiB, 2 concurrent | 2.42 → **5.91 GB/s (2.46x)** | −45.9% | 17.0 MB → 141 KB (−99.2%) | −59.3% | −63.0% |

The way there, the same A/B at each stage: with the codec, CRC and chunks
only (`seq-dotnet-final-defaults.*`) uploads 1.39x / 1.38x, downloads
1.20x / 1.29x; plus the receive window (`seq-dotnet-final-window.*`)
uploads 1.35x / 1.40x, downloads 1.82x / 1.60x; plus 4 connections (the
table). The code changes alone, with equal 64 KiB upload chunks and .NET's
window (`seq-dotnet-final-equal-chunks.*`): downloads +21.7% / +28.5%
(6/6), CPU/GB −23% / −29%; uploads +2.0% (4/6) / +6.2% (5/6), allocation
+1.5% (the write race: 34-38 B per write).

So one .NET client now moves 2.5-3.9x the data on half the CPU per byte,
and its code no longer allocates in proportion to the bytes it moves. The
server's one worker is the limit on downloads now (saturated at ~5.3-5.9
GB/s); with 4 server workers one client reached 11.4 GB/s of downloads
(`screens/seq-dotnet-connections-w4-*`). What the client itself still
spends is mostly the HTTP/2 stack's per-frame work (16 KiB DATA frames,
fixed in .NET, and a cross-thread wakeup per flush), outside the client's
code.

**A hang the bigger chunks exposed, and its fix.** With 1 MiB chunks the
full .NET suite hung in `LimitsAreCheckedLocallyThenByTheServer` (an
upload of 64 MiB + 1 that the server refuses at once) — but only after
other tests of its class had run, never alone. An async dump
(`dotnet-dump` + `dumpasync`) showed the first `WriteAsync` parked in
.NET's `Http2Stream.SendDataAsync`, waiting for flow-control credit: the
first message (1 MiB + framing) exceeded the server's 1 MiB stream window,
and a refusing server never credits the stream again. Neither the
server's `RST_STREAM(NO_ERROR)` nor the transfer's own cancellation
released the write. The client now keeps the first message at 64 KiB of
data (every early refusal is decided on its key and total_size) and races
each pending write against the call's response, abandoning the write once
the server has answered. Pre-fix code: hung in 5 of 5 pair runs; fixed:
3 of 3 passed, then three full suites of 344 tests. Two new tests pin the
promptness contract; neither reproduces the warm-process race on its own
(verified against the pre-fix code, and said so in their remarks).

Host note: during the .NET runs the host was saturated (31.8 of 32
logical CPUs busy: a game, other sessions' test hosts, browsers, and the
VM's own overhead of the client's wake/sleep churn), so the .NET timing
results carry more noise than the server's; allocation counts are exact.
