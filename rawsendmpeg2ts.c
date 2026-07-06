/* rawsendmpeg2ts <file.ts|--stdin> <ip:port>
 *
 * Reads a CBR MPEG-2 TS and emits it over UDP at constant rate, play-once.
 * The rate is derived from two PCRs (~1s apart) on the same PID, then the
 * file is paced out in 7-packet (1316-byte) datagrams against absolute
 * CLOCK_MONOTONIC deadlines. IPv4 only. Multicast destinations get TTL 4.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TS 188
#define PKTS_PER_DG 7
#define DG_BYTES (TS * PKTS_PER_DG)      /* 1316 */
#define PCR_HZ 27000000ULL
#define PCR_MOD ((1ULL << 33) * 300ULL)  /* PCR wraps at 2^33 * 300, not 2^33 */
#define SCAN_SPAN PCR_HZ                 /* require ~1s of PCR span before deriving */

struct prefix {
    uint8_t *data;
    size_t len;
    size_t cap;
};

static void die_errno(const char *msg) {
    perror(msg);
    exit(1);
}

/* Parse "a.b.c.d:port" strictly: IPv4 only (exactly one colon), port 1..65535,
   no trailing garbage. */
static void parse_dest(const char *s, struct sockaddr_in *out) {
    const char *colon = strchr(s, ':');
    if (!colon || strchr(colon + 1, ':')) {
        fprintf(stderr, "destination must be IPv4 ip:port\n");
        exit(1);
    }
    size_t iplen = (size_t)(colon - s);
    char ip[INET_ADDRSTRLEN];
    if (iplen == 0 || iplen >= sizeof(ip)) {
        fprintf(stderr, "invalid IPv4 address\n");
        exit(1);
    }
    memcpy(ip, s, iplen);
    ip[iplen] = '\0';

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &out->sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", ip);
        exit(1);
    }

    errno = 0;
    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "port must be 1..65535\n");
        exit(1);
    }
    out->sin_port = htons((uint16_t)port);
}

/* 27 MHz PCR difference with a single wrap at PCR_MOD. */
static uint64_t pcr_delta(uint64_t later, uint64_t earlier) {
    if (later >= earlier) return later - earlier;
    return later + PCR_MOD - earlier;
}

static void prefix_push(struct prefix *prefix, const uint8_t pkt[TS]) {
    if (prefix->len + TS > prefix->cap) {
        size_t cap = prefix->cap ? prefix->cap * 2 : DG_BYTES;
        while (cap < prefix->len + TS) cap *= 2;
        uint8_t *data = realloc(prefix->data, cap);
        if (!data) die_errno("realloc");
        prefix->data = data;
        prefix->cap = cap;
    }
    memcpy(prefix->data + prefix->len, pkt, TS);
    prefix->len += TS;
}

/* Single pass over the head of the file: lock onto the first PCR PID, then walk
   its PCRs until the value advances ~1s. Returns bits/s (rounded). Exits on a
   malformed TS or if no usable PCR pair exists. */
static uint64_t derive_rate(FILE *f, struct prefix *prefix) {
    uint8_t pkt[TS];
    unsigned long long off = 0;
    int have_base = 0, have_last = 0, found = 0;
    int pid0 = -1;
    unsigned long long B0 = 0, B1 = 0, Blast = 0;
    uint64_t P0 = 0, P1 = 0, Plast = 0;

    for (;;) {
        size_t n = fread(pkt, 1, TS, f);
        if (n == 0) break;
        if (n != TS) {
            fprintf(stderr, "truncated TS packet at offset %llu\n", off);
            exit(1);
        }
        if (pkt[0] != 0x47) {
            fprintf(stderr, "bad sync byte 0x%02x at offset %llu\n", pkt[0], off);
            exit(1);
        }
        if (prefix) prefix_push(prefix, pkt);

        int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
        int afc = (pkt[3] >> 4) & 0x3;
        int disc = 0, is_pcr = 0;
        uint64_t pcr = 0;

        if (afc & 0x2) {                 /* adaptation field present */
            int af_len = pkt[4];
            if (af_len >= 7) {           /* room for the flags byte + 6 PCR bytes */
                int flags = pkt[5];
                disc = (flags & 0x80) != 0;
                if (flags & 0x10) {      /* PCR_flag */
                    uint64_t base = ((uint64_t)pkt[6] << 25) | ((uint64_t)pkt[7] << 17)
                                  | ((uint64_t)pkt[8] << 9)  | ((uint64_t)pkt[9] << 1)
                                  | ((uint64_t)pkt[10] >> 7);
                    uint64_t ext = (((uint64_t)pkt[10] & 0x1) << 8) | pkt[11];
                    pcr = base * 300 + ext;
                    is_pcr = 1;
                }
            }
        }

        if (have_base && pid == pid0 && disc) {
            /* discontinuity on the reference PID: drop the baseline and
               re-establish it strictly after the discontinuity */
            if (is_pcr) { B0 = off; P0 = pcr; have_last = 0; }
            else { have_base = 0; have_last = 0; }
        } else if (is_pcr) {
            if (!have_base) {
                pid0 = pid; B0 = off; P0 = pcr; have_base = 1; have_last = 0;
            } else if (pid == pid0) {
                uint64_t dp = pcr_delta(pcr, P0);
                if (dp > 0) {
                    Blast = off; Plast = pcr; have_last = 1;
                    if (dp >= SCAN_SPAN) { B1 = off; P1 = pcr; found = 1; break; }
                }
            }
        }
        off += TS;
    }

    if (!found) {
        if (have_last) {
            B1 = Blast; P1 = Plast;
            fprintf(stderr, "warning: PCR span under 1s, deriving rate from a %llu-byte window\n",
                    B1 - B0);
        } else {
            fprintf(stderr, "cannot derive rate: need two PCRs on the same PID\n");
            exit(1);
        }
    }

    uint64_t dpll = pcr_delta(P1, P0);
    if (dpll == 0) {
        fprintf(stderr, "cannot derive rate: zero PCR delta\n");
        exit(1);
    }
    unsigned __int128 num = (unsigned __int128)(B1 - B0) * 8u * PCR_HZ;
    uint64_t R = (uint64_t)((num + dpll / 2) / dpll);   /* round, not truncate */
    if (R == 0) {
        fprintf(stderr, "derived rate is zero\n");
        exit(1);
    }
    return R;
}

/* Fill one UDP datagram from the buffered scan prefix, then from the live/file
   input. A pipe may return short reads before EOF, so keep reading until the
   datagram is full or the producer closes. */
static size_t read_datagram(FILE *f, const struct prefix *prefix, size_t *prefix_pos,
                            uint8_t buf[DG_BYTES], unsigned long long *offset) {
    size_t n = 0;
    if (*prefix_pos < prefix->len) {
        size_t take = prefix->len - *prefix_pos;
        if (take > DG_BYTES) take = DG_BYTES;
        memcpy(buf, prefix->data + *prefix_pos, take);
        *prefix_pos += take;
        n += take;
    }

    while (n < DG_BYTES) {
        size_t got = fread(buf + n, 1, DG_BYTES - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) die_errno("fread");
            break;
        }
    }

    if (n % TS != 0) {
        fprintf(stderr, "truncated TS packet at offset %llu\n", *offset + n - n % TS);
        exit(1);
    }
    for (size_t i = 0; i < n; i += TS) {
        if (buf[i] != 0x47) {
            fprintf(stderr, "bad sync byte 0x%02x at offset %llu\n", buf[i], *offset + i);
            exit(1);
        }
    }
    *offset += n;
    return n;
}

static void ts_add_ns(struct timespec *out, const struct timespec *base, uint64_t add_ns) {
    out->tv_sec = base->tv_sec + (time_t)(add_ns / 1000000000ULL);
    out->tv_nsec = base->tv_nsec + (long)(add_ns % 1000000000ULL);
    if (out->tv_nsec >= 1000000000L) {
        out->tv_nsec -= 1000000000L;
        out->tv_sec += 1;
    }
}

/* Pace the whole file out at R bit/s against t0-anchored deadlines. The blocking
   UDP socket provides natural backpressure. Deadlines remain anchored to t0 so
   temporary stalls do not reduce the long-term bitrate. */
static void pump(FILE *f, const struct prefix *prefix, int sock, uint64_t R) {
    uint8_t buf[DG_BYTES];
    struct timespec t0;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) die_errno("clock_gettime");

    uint64_t dg_ns = (uint64_t)((unsigned __int128)DG_BYTES * 8u * 1000000000ULL / R);
    int64_t late_limit = (int64_t)(2 * dg_ns);

    unsigned __int128 sent = 0;
    uint64_t late_count = 0;
    uint64_t max_late_ns = 0;
    uint64_t win_slips = 0, win_max_ns = 0;
    double win_max_at = 0;
    struct timespec next_report;
    size_t prefix_pos = 0;
    unsigned long long input_offset = 0;
    ts_add_ns(&next_report, &t0, 5000000000ULL);   /* first 5s telemetry mark */
    for (;;) {
        size_t n = read_datagram(f, prefix, &prefix_pos, buf, &input_offset);
        if (n < sizeof(buf)) {
            if (n == 0) break;           /* clean EOF */
            /* short final datagram: send the remaining packets */
        }

        uint64_t due_ns = (uint64_t)((unsigned __int128)sent * 8u * 1000000000ULL / R);
        struct timespec deadline;
        ts_add_ns(&deadline, &t0, due_ns);

        int rc;
        while ((rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL)) == EINTR) {
            /* retry on signal */
        }
        if (rc != 0) {
            errno = rc;
            die_errno("clock_nanosleep");
        }

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) die_errno("clock_gettime");
        int64_t late = (int64_t)(now.tv_sec - deadline.tv_sec) * 1000000000LL
                     + (int64_t)(now.tv_nsec - deadline.tv_nsec);
        if (late > late_limit) {
            late_count++;
            win_slips++;
            if ((uint64_t)late > max_late_ns)
                max_late_ns = (uint64_t)late;
            if ((uint64_t)late > win_max_ns) {
                win_max_ns = (uint64_t)late;
                win_max_at = (double)(now.tv_sec - t0.tv_sec)
                           + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
            }
        }

        /* periodic slip telemetry: line up a spike here with an IRD micro-freeze */
        if ((int64_t)(now.tv_sec - next_report.tv_sec) * 1000000000LL
                + (now.tv_nsec - next_report.tv_nsec) >= 0) {
            double elapsed = (double)(now.tv_sec - t0.tv_sec)
                           + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
            fprintf(stderr, "[t+%6.1fs] slips this 5s: %llu (worst %.1f ms at t+%.1fs), total %llu\n",
                    elapsed, (unsigned long long)win_slips, win_max_ns / 1e6, win_max_at,
                    (unsigned long long)late_count);
            win_slips = 0;
            win_max_ns = 0;
            ts_add_ns(&next_report, &next_report, 5000000000ULL);
        }

        ssize_t w = send(sock, buf, n, 0);
        if (w < 0) die_errno("send");
        if ((size_t)w != n) {
            fprintf(stderr, "short send: %zd of %zu bytes\n", w, n);
            exit(1);
        }
        sent += n;
    }

    fprintf(stderr, "done: %llu clock slips, max slip %.3f ms\n",
            (unsigned long long)late_count, max_late_ns / 1e6);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <file.ts|--stdin> <ip:port>   (IPv4 only)\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int stdin_mode = strcmp(path, "--stdin") == 0;

    struct sockaddr_in dst;
    parse_dest(argv[2], &dst);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) die_errno("socket");

    int sndbuf = 32768;   /* small send buffer bounds the post-stall drain burst (matches ffmpeg) */
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) != 0)
        die_errno("SO_SNDBUF");

    if ((ntohl(dst.sin_addr.s_addr) >> 28) == 0xE) {   /* 224.0.0.0/4 */
        int ttl = 4;
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0)
            die_errno("IP_MULTICAST_TTL");
        int loop = 0;
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) != 0)
            die_errno("IP_MULTICAST_LOOP");
    }

    if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) != 0) die_errno("connect");

    FILE *f = stdin_mode ? stdin : fopen(path, "rb");
    if (!f) die_errno(path);

    if (!stdin_mode) {
        if (fseeko(f, 0, SEEK_END) != 0) die_errno("fseeko");
        off_t size = ftello(f);
        if (size < 0) die_errno("ftello");
        if (size == 0 || size % TS != 0) {
            fprintf(stderr, "%s: size %lld is not a positive multiple of 188\n",
                    path, (long long)size);
            return 1;
        }
        if (fseeko(f, 0, SEEK_SET) != 0) die_errno("fseeko");
    }

    struct prefix prefix = {0};
    uint64_t R = derive_rate(f, stdin_mode ? &prefix : NULL);
    fprintf(stderr, "derived rate: %llu bit/s (%.3f Mb/s)\n",
            (unsigned long long)R, R / 1e6);

    if (!stdin_mode && fseeko(f, 0, SEEK_SET) != 0) die_errno("fseeko");
    pump(f, &prefix, sock, R);

    free(prefix.data);
    if (!stdin_mode) fclose(f);
    close(sock);
    return 0;
}
