/*
 * main.c — kvbench entry point: options, phases, server resource accounting
 * and the JSON result.
 *
 * Phases: [preload] → warm-up → measure → stop. Throughput and latency come
 * from the measure window only. The server's CPU time and memory are read
 * from its cgroup (mounted read-only into the kvbench container) at the
 * exact window boundaries, giving CPU efficiency (operations per server
 * CPU-second) and memory per key without trusting any server's self-report.
 */
#include <dirent.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

#include "engine.h"

#define KVBENCH_VERSION "1.0.0"

Options g_opt;
_Atomic int g_phase = PHASE_WARMUP;
uint8_t *g_value_template;
Zipf g_zipf;
static Totals g_totals; /* large (histograms): static, not on the stack */

#define MAX_SAMPLES 3600
static double g_timeline[MAX_SAMPLES]; /* measured-phase throughput per interval */
static int g_nsamples;

/**
 * Prints usage.
 * @param argv0  program name
 */
static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s --protocol resp|memcache|grpc --port P [options]\n"
          "  --host H            server host (127.0.0.1)\n"
          "  --threads T         client threads, each with its own epoll loop (16)\n"
          "  --connections C     total connections (64)\n"
          "  --depth D           max requests in flight per connection (16)\n"
          "  --keys K            key space size (1000000)\n"
          "  --value-size V      value bytes, >= 8 (100)\n"
          "  --read-ratio R      fraction of GETs, rest SETs (1.0)\n"
          "  --ping              protocol no-op instead of GET/SET\n"
          "  --upload            grpc: every request uploads a whole value (client streaming)\n"
          "  --download          grpc: every request downloads a whole value (server streaming;\n"
          "                      use --load: the keys are preloaded with uploads)\n"
          "  --chunk BYTES       streaming: data bytes per Upload message (65536), or the\n"
          "                      Download chunk_size to ask for (server default: 65536)\n"
          "  --zipf THETA        Zipfian keys (YCSB scrambled), 0 < THETA < 1; default uniform\n"
          "  --rate OPS          open loop: Poisson arrivals at OPS/s total (0 = closed loop)\n"
          "  --warmup S          seconds before measuring (5)\n"
          "  --duration S        measured seconds (10)\n"
          "  --load              write all keys first, then measure\n"
          "  --load-only         write all keys, report, exit\n"
          "  --settle S          pause after the load before reading server memory (1)\n"
          "  --cgroup-cpu FILE   server cgroup cpu.stat (v2) or cpuacct.usage (v1)\n"
          "  --server-pid PID    server CPU from /proc/PID/task/*/schedstat instead (ns-exact;\n"
          "                      the server must be in kvbench's PID namespace)\n"
          "  --cgroup-mem FILE   server cgroup memory.stat (v1 or v2)\n"
          "  --wait-ready S      wait up to S seconds for the port to accept (30)\n"
          "  --interval S        throughput timeline sampling period (1)\n"
          "  --busy-poll         client threads never sleep (diagnostic: the server then\n"
          "                      pays no cross-CPU wakeup per send; burns client CPUs)\n"
          "  --seed N            RNG seed (1)\n"
          "  --label TEXT        echoed into the JSON\n",
          argv0);
}

/**
 * Parses a double option or exits.
 * @param s     text
 * @param name  option name for the message
 * @returns the value
 */
static double num(const char *s, const char *name) {
  char *end;
  double v = strtod(s, &end);
  if (*s == 0 || *end != 0) {
    fprintf(stderr, "kvbench: bad value for --%s: %s\n", name, s);
    exit(2);
  }
  return v;
}

/**
 * Parses the command line into g_opt and validates it.
 * @param argc  argument count
 * @param argv  arguments
 * @remarks Exits with status 2 on any invalid combination.
 */
static void parse_options(int argc, char **argv) {
  g_opt = (Options){.host = "127.0.0.1", .threads = 16, .connections = 64, .depth = 16,
                    .keys = 1000000, .value_size = 100, .read_ratio = 1.0, .warmup = 5,
                    .duration = 10, .settle = 1, .seed = 1, .label = "", .wait_ready = 30,
                    .interval = 1, .proto = -1};
  static const struct option longopts[] = {
      {"protocol", 1, 0, 'P'}, {"host", 1, 0, 'h'},      {"port", 1, 0, 'p'},
      {"threads", 1, 0, 't'},  {"connections", 1, 0, 'c'}, {"depth", 1, 0, 'd'},
      {"keys", 1, 0, 'k'},     {"value-size", 1, 0, 'v'}, {"read-ratio", 1, 0, 'r'},
      {"ping", 0, 0, 'n'},     {"zipf", 1, 0, 'z'},       {"rate", 1, 0, 'R'},
      {"warmup", 1, 0, 'w'},   {"duration", 1, 0, 'D'},   {"load", 0, 0, 'l'},
      {"load-only", 0, 0, 'L'}, {"settle", 1, 0, 's'},    {"cgroup-cpu", 1, 0, 'C'},
      {"cgroup-mem", 1, 0, 'M'}, {"seed", 1, 0, 'S'},     {"label", 1, 0, 'b'},
      {"wait-ready", 1, 0, 'W'}, {"interval", 1, 0, 'I'}, {"busy-poll", 0, 0, 'B'},
      {"upload", 0, 0, 'U'},   {"download", 0, 0, 'G'},   {"chunk", 1, 0, 'K'},
      {"server-pid", 1, 0, 'X'}, {"help", 0, 0, '?'},     {0, 0, 0, 0}};
  int ch;
  while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
    switch (ch) {
    case 'P':
      g_opt.proto = !strcmp(optarg, "resp")       ? PROTO_RESP
                    : !strcmp(optarg, "memcache") ? PROTO_MEMCACHE
                    : !strcmp(optarg, "grpc")     ? PROTO_GRPC
                                                  : -2;
      break;
    case 'h': g_opt.host = optarg; break;
    case 'p': g_opt.port = optarg; break;
    case 't': g_opt.threads = (int)num(optarg, "threads"); break;
    case 'c': g_opt.connections = (int)num(optarg, "connections"); break;
    case 'd': g_opt.depth = (int)num(optarg, "depth"); break;
    case 'k': g_opt.keys = (uint64_t)num(optarg, "keys"); break;
    case 'v': g_opt.value_size = (uint32_t)num(optarg, "value-size"); break;
    case 'r': g_opt.read_ratio = num(optarg, "read-ratio"); break;
    case 'n': g_opt.workload = WL_PING; break;
    case 'z': g_opt.zipf = 1; g_opt.zipf_theta = num(optarg, "zipf"); break;
    case 'R': g_opt.rate = num(optarg, "rate"); break;
    case 'w': g_opt.warmup = num(optarg, "warmup"); break;
    case 'D': g_opt.duration = num(optarg, "duration"); break;
    case 'l': g_opt.load = 1; break;
    case 'L': g_opt.load = 2; break;
    case 's': g_opt.settle = num(optarg, "settle"); break;
    case 'C': g_opt.cg_cpu = optarg; break;
    case 'M': g_opt.cg_mem = optarg; break;
    case 'S': g_opt.seed = (uint64_t)num(optarg, "seed"); break;
    case 'b': g_opt.label = optarg; break;
    case 'W': g_opt.wait_ready = num(optarg, "wait-ready"); break;
    case 'I': g_opt.interval = num(optarg, "interval"); break;
    case 'B': g_opt.busy_poll = 1; break;
    case 'X': g_opt.server_pid = (int)num(optarg, "server-pid"); break;
    case 'U': g_opt.workload = WL_UPLOAD; break;
    case 'G': g_opt.workload = WL_DOWNLOAD; break;
    case 'K': {
      double v = num(optarg, "chunk");
      /* Checked here: a negative or huge double would wrap in the cast. */
      if (v < 1 || v > 16777216) {
        fprintf(stderr, "kvbench: --chunk must be 1..16777216\n");
        exit(2);
      }
      g_opt.chunk = (uint32_t)v;
      break;
    }
    default: usage(argv[0]); exit(2);
    }
  }
  const int streaming = g_opt.workload == WL_UPLOAD || g_opt.workload == WL_DOWNLOAD;
  const char *bad = NULL;
  if (g_opt.proto < 0) bad = "--protocol must be resp, memcache or grpc";
  else if (!g_opt.port) bad = "--port is required";
  else if (g_opt.threads < 1 || g_opt.connections < 1) bad = "threads and connections must be >= 1";
  else if (g_opt.depth < 1 || g_opt.depth > 4096) bad = "--depth must be 1..4096";
  else if (g_opt.proto == PROTO_GRPC && g_opt.depth > 100)
    bad = "--depth must be <= 100 for grpc (KvAsm's default concurrent-stream limit)";
  else if (g_opt.keys < 1 || g_opt.keys > 1000000000000ull) bad = "--keys must be 1..10^12";
  else if (g_opt.value_size < 8) bad = "--value-size must be >= 8 (the key id is embedded)";
  else if (g_opt.read_ratio < 0 || g_opt.read_ratio > 1) bad = "--read-ratio must be 0..1";
  else if (g_opt.zipf && !(g_opt.zipf_theta > 0 && g_opt.zipf_theta < 1)) bad = "--zipf must be in (0,1)";
  else if (g_opt.rate < 0 || g_opt.warmup < 0 || g_opt.duration <= 0) bad = "bad rate/warmup/duration";
  else if (g_opt.interval <= 0.01) bad = "--interval must be > 0.01 s";
  else if (streaming && g_opt.proto != PROTO_GRPC) bad = "--upload/--download need --protocol grpc";
  else if (g_opt.chunk && !streaming) bad = "--chunk applies to --upload/--download only";
  if (bad) {
    fprintf(stderr, "kvbench: %s\n", bad);
    exit(2);
  }
  if (g_opt.threads > g_opt.connections) g_opt.threads = g_opt.connections;
}

/**
 * Sleeps for a fractional number of seconds.
 * @param seconds  duration
 */
static void sleep_s(double seconds) {
  struct timespec ts = {.tv_sec = (time_t)seconds,
                        .tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1e9)};
  while (nanosleep(&ts, &ts) != 0) {
  }
}

/**
 * Sleeps until an absolute monotonic time (no drift across samples).
 * @param t  target, nanoseconds of CLOCK_MONOTONIC
 */
static void sleep_until(uint64_t t) {
  struct timespec ts = {.tv_sec = (time_t)(t / 1000000000ull), .tv_nsec = (long)(t % 1000000000ull)};
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {
  }
}

/**
 * Runs the measured phase, sampling throughput every g_opt.interval seconds
 * into g_timeline.
 * @param t1  start of the measured phase (monotonic ns)
 * @remarks Samples are taken at absolute times t1 + k*interval, so sampling
 *          jitter never accumulates; the last interval may be shorter.
 */
static void measure_with_timeline(uint64_t t1) {
  uint64_t prev = engine_progress(), tprev = t1;
  double elapsed = 0;
  while (elapsed + 1e-9 < g_opt.duration) {
    double step = g_opt.interval;
    if (elapsed + step > g_opt.duration) step = g_opt.duration - elapsed;
    elapsed += step;
    sleep_until(t1 + (uint64_t)(elapsed * 1e9));
    uint64_t now = now_ns(), cur = engine_progress();
    if (g_nsamples < MAX_SAMPLES && now > tprev)
      g_timeline[g_nsamples++] = (double)(cur - prev) * 1e9 / (double)(now - tprev);
    prev = cur;
    tprev = now;
  }
}

/**
 * Reads a small text file.
 * @param path  file
 * @param buf   destination
 * @param cap   capacity (NUL-terminated result)
 * @returns bytes read, or -1
 */
static long read_text(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = 0;
  return (long)n;
}

/**
 * Finds "name value" at the start of a line in a stat file.
 * @param text  file contents
 * @param name  field name including the trailing space
 * @param out   receives the value
 * @returns 1 if found
 */
static int stat_field(const char *text, const char *name, int64_t *out) {
  size_t n = strlen(name);
  for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
    if (!strncmp(p, name, n)) {
      *out = strtoll(p + n, NULL, 10);
      return 1;
    }
  }
  return 0;
}

/**
 * CPU time of every live thread of a process: the sum of the first field
 * ("time spent on the cpu", in ns) of /proc/PID/task/TID/schedstat.
 * @param pid  process id (in kvbench's PID namespace)
 * @returns nanoseconds, or -1 when the process is not visible
 * @remarks Exact to the nanosecond where /proc/PID/stat has 10 ms ticks. A
 *          thread that exits takes its time with it, which is fine for
 *          KvAsm (workers live as long as the process). Over loopback the
 *          server's figure includes the peer's TCP receive processing run
 *          in the server's send() (and loses its own, run in the client's):
 *          compare variants measured the same way, never against a NIC.
 */
static int64_t pid_cpu_ns(int pid) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%d/task", pid);
  DIR *dir = opendir(path);
  if (!dir) return -1;
  int64_t sum = 0;
  struct dirent *e;
  while ((e = readdir(dir)) != NULL) {
    if (e->d_name[0] == '.') continue;
    /* d_name can be 255 bytes in general (a TID is at most 10 digits here),
     * so the path buffer is sized for the general case: no truncation. */
    char file[300], buf[128];
    snprintf(file, sizeof file, "/proc/%d/task/%s/schedstat", pid, e->d_name);
    if (read_text(file, buf, sizeof buf) > 0) sum += strtoll(buf, NULL, 10);
  }
  closedir(dir);
  return sum;
}

/**
 * Server CPU time consumed so far.
 * @returns nanoseconds, or -1 without --cgroup-cpu / --server-pid or when
 *          unreadable
 * @remarks --server-pid wins; cgroup v2 cpu.stat has "usage_usec N"; v1
 *          cpuacct.usage is a bare nanosecond count.
 */
static int64_t server_cpu_ns(void) {
  char buf[4096];
  if (g_opt.server_pid > 0) return pid_cpu_ns(g_opt.server_pid);
  if (!g_opt.cg_cpu || read_text(g_opt.cg_cpu, buf, sizeof buf) < 0) return -1;
  int64_t v;
  if (stat_field(buf, "usage_usec ", &v)) return v * 1000;
  return strtoll(buf, NULL, 10);
}

/**
 * Server anonymous memory (what an in-memory store actually holds).
 * @returns bytes, or -1 without --cgroup-mem or when unreadable
 * @remarks v2 memory.stat "anon", v1 memory.stat "rss" (anonymous memory,
 *          transparent huge pages included); page cache is excluded because
 *          none of the systems does file I/O in these runs.
 */
static int64_t server_mem_bytes(void) {
  static char buf[16384];
  if (!g_opt.cg_mem || read_text(g_opt.cg_mem, buf, sizeof buf) < 0) return -1;
  int64_t v;
  if (stat_field(buf, "anon ", &v) || stat_field(buf, "rss ", &v)) return v;
  return -1;
}

/**
 * Process CPU time (all kvbench threads).
 * @returns user + system seconds
 */
static double client_cpu_s(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 +
         (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
}

/**
 * Prints a latency object in microseconds.
 * @param h  histogram
 */
static void print_latency(const Hist *h) {
  printf("{\"count\":%" PRIu64 ",\"mean\":%.2f,\"p50\":%.2f,\"p90\":%.2f,\"p99\":%.2f,"
         "\"p999\":%.2f,\"p9999\":%.2f,\"max\":%.2f}",
         h->total, h->total ? h->sum / (double)h->total / 1000.0 : 0.0,
         hist_percentile(h, 50) / 1000.0, hist_percentile(h, 90) / 1000.0,
         hist_percentile(h, 99) / 1000.0, hist_percentile(h, 99.9) / 1000.0,
         hist_percentile(h, 99.99) / 1000.0, h->max / 1000.0);
}

/**
 * Prints a JSON string with the minimal escaping kvbench's texts need.
 * @param s  text
 */
static void print_string(const char *s) {
  putchar('"');
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') putchar('\\');
    putchar((unsigned char)*s < 0x20 ? ' ' : *s);
  }
  putchar('"');
}

/**
 * Prints the error field and returns the exit code for a totals record.
 * @param t  totals
 * @returns 0 when the run is valid, 1 otherwise
 */
static int print_validity(const Totals *t) {
  int valid = t->errors == 0 && t->misses == 0 && t->bad_values == 0;
  printf("\"valid\":%s,\"errors\":%" PRIu64 ",\"misses\":%" PRIu64 ",\"bad_values\":%" PRIu64
         ",\"error_text\":",
         valid ? "true" : "false", t->errors, t->misses, t->bad_values);
  print_string(t->error_text);
  return valid ? 0 : 1;
}

/**
 * Entry point: see the file header.
 * @param argc  argument count
 * @param argv  arguments
 * @returns 0 for a valid run, 1 for an invalid one (errors, misses, wrong
 *          values), 2 for usage or setup failures
 */
int main(int argc, char **argv) {
  parse_options(argc, argv);
  g_value_template = malloc(g_opt.value_size);
  for (uint32_t i = 0; i < g_opt.value_size; i++) g_value_template[i] = (uint8_t)('a' + i % 26);
  proto_text_init();
  value_crc_init(); /* before any worker: they read its results unsynchronized */
  if (g_opt.zipf) zipf_init(&g_zipf, g_opt.keys, g_opt.zipf_theta);
  if (engine_resolve() != 0) return 2;
  if (engine_wait_ready(g_opt.wait_ready) != 0) {
    printf("{\"tool\":\"kvbench\",\"valid\":false,\"error_text\":\"server not reachable\"}\n");
    return 2;
  }

  static const char *proto_names[] = {"resp", "memcache", "grpc"};
  static const char *workload_names[] = {"mixed", "ping", "upload", "download"};
  printf("{\"tool\":\"kvbench\",\"version\":\"" KVBENCH_VERSION "\",\"label\":");
  print_string(g_opt.label);
  printf(",\"config\":{\"protocol\":\"%s\",\"threads\":%d,\"connections\":%d,\"depth\":%d,"
         "\"keys\":%" PRIu64 ",\"key_size\":%d,\"value_size\":%u,\"read_ratio\":%.4f,"
         "\"workload\":\"%s\",\"chunk\":%u,\"distribution\":\"%s\",\"zipf_theta\":%.3f,"
         "\"rate\":%.0f,\"warmup_s\":%.2f,\"duration_s\":%.2f,\"seed\":%" PRIu64
         ",\"busy_poll\":%s}",
         proto_names[g_opt.proto], g_opt.threads, g_opt.connections, g_opt.depth, g_opt.keys,
         KEY_BYTES, g_opt.value_size, g_opt.read_ratio, workload_names[g_opt.workload],
         g_opt.chunk, g_opt.zipf ? "zipf" : "uniform", g_opt.zipf_theta, g_opt.rate,
         g_opt.warmup, g_opt.duration, g_opt.seed, g_opt.busy_poll ? "true" : "false");

  int rc = 0;
  if (g_opt.load) {
    int64_t mem_before = server_mem_bytes();
    uint64_t t0 = now_ns();
    int started = engine_start(1);
    engine_join(&g_totals);
    double secs = (double)(now_ns() - t0) / 1e9;
    if (g_opt.settle > 0) sleep_s(g_opt.settle);
    int64_t mem_after = server_mem_bytes();
    printf(",\"preload\":{\"keys\":%" PRIu64 ",\"seconds\":%.3f,\"ops_per_sec\":%.0f,"
           "\"server_mem_before\":%" PRId64 ",\"server_mem_after\":%" PRId64 ",",
           g_opt.keys, secs, (double)g_totals.done_all / secs, mem_before, mem_after);
    rc = print_validity(&g_totals);
    printf("}");
    if (started != 0 || g_totals.done_all != g_opt.keys) rc = 1;
  }

  if (g_opt.load != 2 && rc == 0) {
    int started = engine_start(0);
    sleep_s(g_opt.warmup);
    double cpu1 = client_cpu_s();
    int64_t srv1 = server_cpu_ns();
    uint64_t t1 = now_ns();
    atomic_store(&g_phase, PHASE_MEASURE);
    measure_with_timeline(t1);
    atomic_store(&g_phase, PHASE_STOP);
    uint64_t t2 = now_ns();
    int64_t srv2 = server_cpu_ns();
    double cpu2 = client_cpu_s();
    int64_t mem = server_mem_bytes();
    engine_join(&g_totals);

    double secs = (double)(t2 - t1) / 1e9;
    uint64_t ops = 0;
    for (int op = 0; op < OP_COUNT; op++) ops += g_totals.done[op];
    /* Value bytes moved by streaming calls (each moves one whole value). */
    uint64_t transfers = g_totals.done[OP_UPLOAD] + g_totals.done[OP_DOWNLOAD];
    double value_bytes_per_sec = (double)transfers * g_opt.value_size / secs;
    Hist all = {0};
    for (int op = 0; op < OP_COUNT; op++) hist_merge(&all, &g_totals.hist[op]);
    double srv_cpu = srv1 >= 0 && srv2 >= 0 ? (double)(srv2 - srv1) / 1e9 : -1;
    printf(",\"result\":{\"seconds\":%.3f,\"ops\":%" PRIu64 ",\"ops_per_sec\":%.0f,"
           "\"gets\":%" PRIu64 ",\"sets\":%" PRIu64 ",\"pings\":%" PRIu64 ",\"uploads\":%" PRIu64
           ",\"downloads\":%" PRIu64 ",\"value_bytes_per_sec\":%.0f,\"stalls\":%" PRIu64
           ",\"client_cpu_s\":%.3f,\"client_cpu_util\":%.3f,\"server_cpu_s\":%.3f,"
           "\"server_mem_bytes\":%" PRId64 ",\"latency_us\":",
           secs, ops, (double)ops / secs, g_totals.done[OP_GET], g_totals.done[OP_SET],
           g_totals.done[OP_PING], g_totals.done[OP_UPLOAD], g_totals.done[OP_DOWNLOAD],
           value_bytes_per_sec, g_totals.stalls, cpu2 - cpu1,
           (cpu2 - cpu1) / (secs * g_opt.threads), srv_cpu, mem);
    print_latency(&all);
    printf(",\"get_latency_us\":");
    print_latency(&g_totals.hist[OP_GET]);
    printf(",\"set_latency_us\":");
    print_latency(&g_totals.hist[OP_SET]);
    printf(",\"upload_latency_us\":");
    print_latency(&g_totals.hist[OP_UPLOAD]);
    printf(",\"download_latency_us\":");
    print_latency(&g_totals.hist[OP_DOWNLOAD]);
    printf(",\"interval_s\":%.3f,\"timeline_ops_per_sec\":[", g_opt.interval);
    for (int i = 0; i < g_nsamples; i++) printf("%s%.0f", i ? "," : "", g_timeline[i]);
    printf("],");
    rc = print_validity(&g_totals);
    printf("}");
    if (started != 0 || ops == 0) rc = 1;
  }
  printf("}\n");
  return rc;
}
