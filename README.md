# rawsendmpeg2ts

Plays a CBR MPEG-TS file once over UDP, with no remux and no payload modification. It is a minimal test tool for feeding a hardware IRD with uniformly spaced datagrams. The program is written in C11 and uses only POSIX and IPv4 sockets. It is not intended to be a production sender nor a general MPEG-TS tool.

<img width="543" height="461" alt="Captura de pantalla_20260703_183618" src="https://github.com/user-attachments/assets/38ed49d3-7b1d-4b70-b5a7-baf8347c697f" />


## Build

```bash
cmake -B build
cmake --build build

# The binary is produced at `build/rawsendmpeg2ts`.
$ rawsendmpeg2ts <file.ts|--stdin> <ip:port>
```

Example:

```bash
./build/rawsendmpeg2ts /path/to/golden.ts 239.1.74.20:9009
```

A null-stuffed CBR TS may also arrive over stdin. The sender keeps in memory the segment of
approximately one second used to derive the rate, and then continues reading from the pipe:

```bash
cat /path/to/golden.ts |
  ./build/rawsendmpeg2ts --stdin 239.1.74.20:9009
```

To convert the non-stuffed TS output of MoQ into CBR before UDP pacing:

```bash
/home/ariel/projects/moq-dev.main/target/release/moq \
    --client-connect https://192.168.99.6 \
    --client-tls-disable-verify \
    --broadcast test/bbb \
    export ts |
  ffmpeg -hide_banner -loglevel warning \
    -i pipe:0 -map 0:v:0 -map 0:a:0 -c copy \
    -muxrate 11M -pcr_period 20 -f mpegts pipe:1 |
  ./build/rawsendmpeg2ts --stdin 239.1.74.20:9009
```

FFmpeg performs the remux and adds null stuffing. `rawsendmpeg2ts` derives the resulting muxrate and
fixes the UDP cadence. Do not use `-re` in this pipeline: the raw sender is the sole owner of the
final pacing.

The destination must be IPv4. For multicast, TTL 4 is configured and multicast loopback is disabled.
The egress interface is decided by the operating system route:

```bash
ip route get 239.1.74.20
```

## What it does

1. Verifies that the file is a complete sequence of 188-byte TS packets.
2. Finds the first PID carrying PCR.
3. Measures approximately one second of PCR separation and derives the rate from bytes/PCR.
4. Rewinds the file, or replays the stored prefix when the input is stdin.
5. Sends seven TS packets per UDP datagram, 1316 bytes, against absolute `CLOCK_MONOTONIC`
   deadlines.
6. Terminates at EOF. It does not loop.

The socket is blocking and uses `SO_SNDBUF=32768`. Deadlines remain anchored to the start so that a
temporary delay does not reduce the long-term rate.

Every five seconds the program reports the wakes that arrived more than two datagram intervals late.
These are telemetry, not an abort condition:

```text
[t+ 500.0s] slips this 5s: 3 (worst 1.9 ms at t+496.6s), total 303
```

A small, bounded number of slips is normal under the Linux scheduler. What matters is growing delay,
sustained backpressure, or a visible correlation with IRD freezes.

## What it does not do

- It does not remux, stuff, drop, or rewrite TS packets.
- It does not correct PCR, PTS, DTS, continuity counters, or T-STD.
- It does not support VBR as a general case. The rate is derived assuming a CBR relationship between
  PCR and byte position.
- It does not select the multicast interface explicitly.
- It does not support IPv6, loop playback, or multiple configurable programs.
- It does not configure `SCHED_FIFO`, CPU affinity, or `mlockall`.

## Required test bed

Validation was performed over a direct link (switches may apply multicast storm control and
invalidate the test: they produce false _pacing_ and it appears to pass, or they stop the _storm_ and
it appears to fail).

**Stop the suffering: connect a direct cable to the IRD**:

```text
PC/NIC -> Ethernet cable -> Sencore
```
## Audio? Yes.
Testing with audio is important; clicks and micro-silences quickly expose problems that video alone
hides.

**Stop the suffering: connect a speaker**:

## Switches / network

There must be no switch between the sender and the IRD. The PC and the Sencore need static addresses
in the same subnet. Multicast works over the direct link, and the route must point to the NIC
connected to the Sencore.

Before each test:

```bash
ip route get 239.1.74.20
ethtool <interface> | grep -E 'Speed|Duplex|Link detected'
ethtool -S <interface> | grep -E 'rx_flow_control_xon|rx_flow_control_xoff'
```

During the run, `rx_flow_control_xoff` must remain stable. The result is evaluated over the complete
file, not only during a smoke test: clean A/V playback, normal termination, no growing delay, and no
sustained backpressure.

### Why a switch invalidates the test

A switch may apply storm control, multicast policing, rate limiting, shaping, or 802.3x PAUSE. That
can change the rate and the cadence even if the port negotiates at 100 Mb/s or 1 Gb/s. The sender
then measures the behavior of the path and not only that of the file, its scheduler, and the IRD.

On this bench, the path through a switch produced the following false diagnosis:

- The TS declared 11 Mb/s, but FFmpeg took 689.866 s to transport 596.459 s of media.
- Actual UDP throughput was 9.511 Mb/s, almost exactly a 10 Mb/s Ethernet limit after overhead.
- `rx_flow_control_xoff` grew about 65 times per second.
- `rawsendmpeg2ts` accumulated delay and the IRD showed micro-freezes.
- With a direct cable, the same TS ran to completion at 11 Mb/s, with no XOFF and perfect A/V.
- The direct link also played a 20 Mb/s broadcast TS correctly.

For that reason a test behind a switch is diagnostic, not a Gate. If the result differs from the
direct cable, storm control, multicast policing, flow control, and port configuration must be
reviewed.

## Local demonstration without an IRD

`udp_sink.py` receives datagrams, measures throughput and inter-arrival time, and can save a prefix
to test byte fidelity.

```bash
python3 udp_sink.py \
  --port 5004 \
  --seconds 9 \
  --save rx.bin \
  --stats stats.json &

./build/rawsendmpeg2ts /path/to/golden.ts 127.0.0.1:5004

cmp -n 1000000 rx.bin /path/to/golden.ts
cat stats.json
```

Loopback serves as a smoke test. Measuring real pacing requires a capture on the receiver side.

## Stuffed CBR goldens of Big Buck Bunny

All goldens used on the bench are H.264, 1920x1080i59.94 top-field-first, AAC 5.1 at 48 kHz, PCR
every 20 ms, and CBR MPEG-TS with null stuffing.

Common source:

```text
/home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov
```

The original `.mov` was downloaded from Blender's official Big Buck Bunny movie archive:

```text
https://download.blender.org/peach/bigbuckbunny_movies/
```

The `.ts` files are outside the repository. The recipes require FFmpeg with `libx264`.


```bash
### Generate 4 Mb/s
ffmpeg -hide_banner -y \
  -i /home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "fps=30000/1001" \
  -c:v libx264 -preset veryfast -profile:v main -level:v 4.1 -pix_fmt yuv420p \
  -flags +ilme+ildct \
  -x264opts "interlaced=1:tff=1:nal-hrd=cbr:keyint=30" \
  -b:v 3M -minrate 3M -maxrate 3M -bufsize 3M \
  -c:a aac -b:a 160k \
  -muxrate 4M -pcr_period 20 \
  -f mpegts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-4m.ts

### Generate 8 Mb/s
ffmpeg -hide_banner -y \
  -i /home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "fps=30000/1001" \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p \
  -flags +ilme+ildct \
  -x264opts "interlaced=1:tff=1:nal-hrd=cbr:keyint=30" \
  -b:v 6M -minrate 6M -maxrate 6M -bufsize 6M \
  -c:a copy \
  -muxrate 8M -pcr_period 20 \
  -f mpegts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-8m.ts

### Generate 11 Mb/s
ffmpeg -hide_banner -y \
  -i /home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "fps=30000/1001" \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p \
  -flags +ilme+ildct \
  -x264opts "interlaced=1:tff=1:nal-hrd=cbr:keyint=30" \
  -b:v 8M -minrate 8M -maxrate 8M -bufsize 8M \
  -c:a copy \
  -muxrate 11M -pcr_period 20 \
  -f mpegts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i-clean.ts

### Generate 20 Mb/s
ffmpeg -hide_banner -y \
  -i /home/ariel/projects/moq-dev/notes/captures/big_buck_bunny_1080p_h264.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "fps=30000/1001" \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p \
  -flags +ilme+ildct \
  -x264opts "interlaced=1:tff=1:nal-hrd=cbr:keyint=30" \
  -b:v 17M -minrate 17M -maxrate 17M -bufsize 17M \
  -c:a copy \
  -muxrate 20M -pcr_period 20 \
  -f mpegts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-20m.ts
```

## Emitting the golden specimens via rawsendmpeg2ts

```bash
### 4 Mb/s
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-4m.ts \
  239.1.74.20:9009

### 8 Mb/s
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-8m.ts \
  239.1.74.20:9009

### 11 Mb/s
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i-clean.ts \
  239.1.74.20:9009

### 20 Mb/s
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-20m.ts \
  239.1.74.20:9009
```

## Emitting with TSDuck

The four cases also produced correct A/V on the IRD with native TSDuck over a direct cable. The local
address pins the egress to NIC `10.6.6.1` and prevents multicast from using another interface.

```bash
### 4 Mb/s
tsp \
  -I file /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-4m.ts \
  -P regulate --bitrate 4000000 --packet-burst 7 \
  -O ip --local-address 10.6.6.1 --ttl 4 --packet-burst 7 \
  239.1.74.20:9009

### 8 Mb/s
tsp \
  -I file /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-8m.ts \
  -P regulate --bitrate 8000000 --packet-burst 7 \
  -O ip --local-address 10.6.6.1 --ttl 4 --packet-burst 7 \
  239.1.74.20:9009

### 11 Mb/s
tsp \
  -I file /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994.ts \
  -P regulate --bitrate 11000000 --packet-burst 7 \
  -O ip --local-address 10.6.6.1 --ttl 4 --packet-burst 7 \
  239.1.74.20:9009

### 20 Mb/s
tsp \
  -I file /home/ariel/projects/moq-dev/notes/captures/golden-bbb-1080i5994-20m.ts \
  -P regulate --bitrate 20000000 --packet-burst 7 \
  -O ip --local-address 10.6.6.1 --ttl 4 --packet-burst 7 \
  239.1.74.20:9009
```

## Verified artifacts

| Mux | File | Size | Measured bitrate | Null packets | SHA-256 |
|---:|---|---:|---:|---:|---|
| 4 Mb/s | `golden-bbb-1080i5994-4m.ts` | 298,232,672 bytes | 3,999,875 bit/s | 268,589 (16.9%) | `b2374b2982f160873c784fe14ca0184bc9913a4859da4c746d0dafef9c04ff7e` |
| 8 Mb/s | `golden-bbb-1080i5994-8m.ts` | 596,506,140 bytes | 8,000,584 bit/s | 522,046 (16.5%) | `72deb806f6fcf7f0e42fa723febadd82bbe167442c0bf0a7641987df88edc874` |
| 11 Mb/s | `golden-bbb-1080i-clean.ts` | 820,195,308 bytes | 11,000,795 bit/s | 901,466 (20.7%) | `81d4b5ab2613661e53e95d88e8f32f33b83272335cd33d288bc7b9a281817560` |
| 20 Mb/s | `golden-bbb-1080i5994-20m.ts` | 1,491,262,248 bytes | 20,001,550 bit/s | 828,215 (10.4%) | `c10208f2a135fdddefb32c42a0a4142db309c3d02fe4399d7e2721a186891723` |

The hashes identify the artifacts used on the bench. Different versions of FFmpeg or `libx264` may
produce a different file using the same parameters.

## Direct results with the Sencore

| Mux | Run | Slips | Result |
|---:|---|---:|---|
| 4 Mb/s | partial | 0 | perfect A/V |
| 8 Mb/s | partial | 0 | perfect A/V |
| 11 Mb/s | complete, ~596 s | 19, maximum 7.966 ms | perfect A/V |
| 20 Mb/s | complete, ~596 s | 306, maximum 7.294 ms | perfect A/V |

In the 20 Mb/s run, 290 of the 306 slips were concentrated in a single episode of about 15 seconds.
There was no cumulative delay and no visible effect on A/V.

Native TSDuck also produced correct A/V at 4, 8, 11, and 20 Mb/s using `regulate` and bursts of seven
packets. On the direct Gate, `rawsendmpeg2ts` achieves the same IRD acceptance as TSDuck.

Complete logs:

```text
logs/full-bbb.log
  logs/full-bbb-20m.log
```

## Comparison with FFmpeg

- The same 4, 8, and 11 Mb/s CBR streams produced glitches with FFmpeg `-re` over a direct cable.
- All three worked with `rawsendmpeg2ts` and zero slips in the comparative tests.
- The 4 Mb/s remux produced by FFmpeg worked when played back with `rawsendmpeg2ts`. This separated
  the remuxed content from the UDP cadence.
- FFmpeg regulates reading by timestamps, but its UDP writes occur in groups and with variable sizes.
  That is not equivalent to spacing every TS datagram uniformly.

The switch limitation and the FFmpeg cadence were independent problems. The switch explained the
backpressure at 11 Mb/s, but not the FFmpeg glitches, which also appeared at 4 and 8 Mb/s over a
direct cable.

## MOV & TS Across MoQ tests with IRD-compliant output

Three sources were tested end-to-end through MoQ: a live re-encoded MOV, a raw 1080i59.94 TS, and a
European 1080i50 TS. In all three cases, MoQ transported VBR/non-stuffed media and the egress
reconstructed a null-stuffed 11 Mb/s CBR MPEG-TS. All three produced clean audio and video on the
Sencore over a direct cable.

### MOV ingress with AAC 5.1 audio

```bash
ffmpeg -re -stream_loop -1 \
  -i /home/ariel/projects/poky/downloads/bigbuckbunny1080p.mov \
  -map 0:v:0 -map 0:a:0 \
  -vf "scale=1920:1080,fps=60000/1001,interlace=scan=tff:lowpass=1,setfield=tff,format=yuv420p" \
  -c:v libx264 \
  -profile:v high \
  -level 4.0 \
  -preset veryfast \
  -x264opts "interlaced=1:tff=1:weightp=0" \
  -b:v 9M \
  -maxrate 9M \
  -bufsize 1M \
  -g 30 \
  -bf 2 \
  -c:a copy \
  -f mpegts - |
/home/ariel/projects/moq-dev/target/release/moq-cli publish \
  --url https://192.168.99.6 \
  --client-tls-disable-verify \
  --broadcast test/bbb \
  ts
```

### Raw 1080i59.94 TS ingress

```bash
ffmpeg -re -stream_loop -1 \
  -i /home/ariel/projects/poky/downloads/bbb_1080i5994.ts \
  -map 0 \
  -c copy \
  -f mpegts pipe:1 |
/home/ariel/projects/moq-dev.main/target/release/moq \
  --client-connect https://192.168.99.6 \
  --client-tls-disable-verify \
  --broadcast test/bbb \
  import ts
```

### Raw European 1080i50 TS ingress

```bash
ffmpeg -re -stream_loop -1 \
  -i /home/ariel/projects/moq-dev/notes/captures/CNNiEMEA2.ts \
  -map 0 \
  -c copy \
  -f mpegts pipe:1 |
/home/ariel/projects/moq-dev.main/target/release/moq \
  --client-connect https://192.168.99.6 \
  --client-tls-disable-verify \
  --broadcast test/bbb \
  import ts
```

Both TS commands use `-c copy`, so they preserve the encoded video, framerate, field order, audio,
and temporal relationships without re-encoding. By omitting `-muxrate`, the TS delivered to the
importer is VBR/non-stuffed. The MoQ importer discards any ingress null stuffing in any case.

### Common egress toward the Sencore

```bash
/home/ariel/projects/moq-dev.main/target/release/moq \
  --client-connect https://192.168.99.6 \
  --client-tls-disable-verify \
  --broadcast test/bbb \
  export ts |
ffmpeg -hide_banner -loglevel warning \
  -i pipe:0 \
  -map 0:v:0 \
  -map '0:a:0?' \
  -c copy \
  -muxrate 11M \
  -pcr_period 20 \
  -f mpegts pipe:1 |
/home/ariel/projects/rawsendmpeg2ts/build/rawsendmpeg2ts \
  --stdin 239.1.74.20:9009
```

At the egress, FFmpeg recreates the CBR multiplex and its null packets. `rawsendmpeg2ts` preserves
that TS byte for byte and applies the uniform UDP pacing the IRD requires.
