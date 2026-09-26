/*
 * proto.c — the two request/response-ordered text protocols:
 *   RESP (Redis, Valkey, Dragonfly, Garnet) and memcached's meta protocol.
 *
 * Both are pipelined: responses arrive in request order, so each connection
 * keeps a FIFO of in-flight requests. Parsing is incremental and in place
 * (the engine receives straight into Conn.in): a response may be split
 * across reads at any byte, and a partial response simply waits for more.
 * Every GET value is verified byte for byte against the expected content.
 */
#include <stdio.h>
#include <stdlib.h>

#include "kvbench.h"

/* Pre-rendered length headers for the configured value size. */
static char g_resp_value_header[24];      /* "$<V>\r\n" */
static size_t g_resp_value_header_len;
static char g_mc_value_suffix[24];        /* " <V>\r\n" */
static size_t g_mc_value_suffix_len;

/**
 * Renders the per-run constant length headers; call once after options parse.
 */
void proto_text_init(void) {
  g_resp_value_header_len = (size_t)snprintf(g_resp_value_header, sizeof g_resp_value_header,
                                             "$%u\r\n", g_opt.value_size);
  g_mc_value_suffix_len = (size_t)snprintf(g_mc_value_suffix, sizeof g_mc_value_suffix,
                                           " %u\r\n", g_opt.value_size);
}

/**
 * Parses a decimal integer (optionally negative) that ends exactly at end.
 * @param p    first byte
 * @param end  one past the last digit
 * @param out  receives the value
 * @returns 0, or -1 when empty, not a number or too long
 */
static int parse_ll(const uint8_t *p, const uint8_t *end, long long *out) {
  int neg = 0;
  if (p < end && *p == '-') {
    neg = 1;
    p++;
  }
  if (p == end || end - p > 18) return -1;
  long long v = 0;
  for (; p < end; p++) {
    if (*p < '0' || *p > '9') return -1;
    v = v * 10 + (*p - '0');
  }
  *out = neg ? -v : v;
  return 0;
}

/**
 * Grows the receive buffer so a response of `need` total bytes fits.
 * @param c     connection
 * @param need  bytes the pending response occupies from the buffer start
 */
static void in_fit(Conn *c, size_t need) {
  if (need + 65536 <= c->in_cap) return;
  size_t cap = c->in_cap * 2;
  while (cap < need + 65536) cap *= 2;
  uint8_t *p = realloc(c->in, cap);
  if (!p) {
    conn_fail(c, "out of memory (receive buffer)");
    return;
  }
  c->in = p;
  c->in_cap = cap;
}

/**
 * Drops parsed bytes, keeping a trailing partial response at the start.
 * @param c     connection
 * @param used  bytes consumed from the buffer start
 */
static void in_consume(Conn *c, size_t used) {
  if (used == 0) return;
  memmove(c->in, c->in + used, c->in_len - used);
  c->in_len -= used;
}

/**
 * Records a protocol violation and stops the connection.
 * @param c     connection
 * @param what  description
 * @returns -1 (for tail calls from parsers)
 */
static int proto_error(Conn *c, const char *what) {
  conn_fail(c, what);
  return -1;
}

/* ------------------------------------------------------------------------- */
/* RESP                                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Queues one RESP command (RESP2 multi-bulk, as redis-benchmark sends).
 * @param c    connection
 * @param op   OP_GET / OP_SET / OP_PING
 * @param key  key id
 * @param t0   start time
 */
static void resp_issue(Conn *c, int op, uint64_t key, uint64_t t0) {
  if (op == OP_PING) {
    out_put(c, "*1\r\n$4\r\nPING\r\n", 14);
  } else {
    out_reserve(c, 64 + g_opt.value_size);
    uint8_t *p = c->out + c->out_len;
    memcpy(p, op == OP_GET ? "*2\r\n$3\r\nGET\r\n$16\r\n" : "*3\r\n$3\r\nSET\r\n$16\r\n", 18);
    key_format((char *)p + 18, key);
    memcpy(p + 34, "\r\n", 2);
    size_t n = 36;
    if (op == OP_SET) {
      memcpy(p + n, g_resp_value_header, g_resp_value_header_len);
      n += g_resp_value_header_len;
      value_fill(p + n, key);
      n += g_opt.value_size;
      memcpy(p + n, "\r\n", 2);
      n += 2;
    }
    c->out_len += n;
  }
  fifo_push(c, op, key, t0);
}

/**
 * Parses RESP replies: bulk string (GET hit), null bulk (miss), simple string
 * (+OK / +PONG) and errors.
 * @param c     connection
 * @param data  unused: bytes were received in place at c->in + c->in_len
 * @param len   number of new bytes
 * @param now   receive time (completion timestamp)
 * @returns 0, or -1 on a protocol violation
 */
static int resp_input(Conn *c, const uint8_t *data, size_t len, uint64_t now) {
  (void)data;
  c->in_len += len;
  size_t pos = 0;
  while (pos < c->in_len) {
    const uint8_t *p = c->in + pos, *end = c->in + c->in_len;
    const uint8_t *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) break; /* header line incomplete */
    if (nl == p || nl[-1] != '\r') return proto_error(c, "RESP: malformed line");
    size_t line = (size_t)(nl + 1 - p);
    Pending req;
    int status;
    size_t used = line;
    if (p[0] == '$') {
      long long vlen;
      if (parse_ll(p + 1, nl - 1, &vlen) < 0) return proto_error(c, "RESP: bad bulk length");
      if (vlen < 0) {
        status = ST_MISS;
      } else {
        used = line + (size_t)vlen + 2;
        if ((size_t)(end - p) < used) {
          in_fit(c, used);
          break; /* body incomplete */
        }
        if (!fifo_pop(c, &req)) return proto_error(c, "RESP: unsolicited reply");
        status = req.op == OP_GET && value_ok(req.key, nl + 1, (size_t)vlen) ? ST_OK : ST_BAD_VALUE;
        complete(c, req.op, req.key, req.t0, now, status);
        pos += used;
        continue;
      }
    } else if (p[0] == '+') {
      status = ST_OK; /* +OK (SET), +PONG (PING) */
    } else if (p[0] == '-') {
      if (!c->w->error_text[0])
        snprintf(c->w->error_text, sizeof c->w->error_text, "server error: %.*s", (int)(line - 2), p);
      status = ST_ERROR;
    } else {
      return proto_error(c, "RESP: unexpected reply type");
    }
    if (!fifo_pop(c, &req)) return proto_error(c, "RESP: unsolicited reply");
    if (status == ST_OK && req.op == OP_GET) status = ST_BAD_VALUE; /* GET must be bulk */
    complete(c, req.op, req.key, req.t0, now, status);
    pos += used;
  }
  in_consume(c, pos);
  return 0;
}

const Proto proto_resp = {
    .name = "resp",
    .issue = resp_issue,
    .input = resp_input,
    .parses_in_place = 1,
};

/* ------------------------------------------------------------------------- */
/* memcached meta protocol                                                    */
/* ------------------------------------------------------------------------- */

/**
 * Queues one meta command: "mg <key> v", "ms <key> <len>" + data, or "mn".
 * The meta protocol is memcached's current recommended protocol (compact
 * replies, no key echo), the fairest choice for it.
 * @param c    connection
 * @param op   OP_GET / OP_SET / OP_PING
 * @param key  key id
 * @param t0   start time
 */
static void mc_issue(Conn *c, int op, uint64_t key, uint64_t t0) {
  if (op == OP_PING) {
    out_put(c, "mn\r\n", 4);
  } else {
    out_reserve(c, 64 + g_opt.value_size);
    uint8_t *p = c->out + c->out_len;
    memcpy(p, op == OP_GET ? "mg " : "ms ", 3);
    key_format((char *)p + 3, key);
    size_t n = 3 + KEY_BYTES;
    if (op == OP_GET) {
      memcpy(p + n, " v\r\n", 4);
      n += 4;
    } else {
      memcpy(p + n, g_mc_value_suffix, g_mc_value_suffix_len);
      n += g_mc_value_suffix_len;
      value_fill(p + n, key);
      n += g_opt.value_size;
      memcpy(p + n, "\r\n", 2);
      n += 2;
    }
    c->out_len += n;
  }
  fifo_push(c, op, key, t0);
}

/**
 * Parses meta replies: "VA <len>" + data (hit), "EN" (miss), "HD" (stored),
 * "MN" (no-op), and error lines.
 * @param c     connection
 * @param data  unused: bytes were received in place
 * @param len   number of new bytes
 * @param now   receive time
 * @returns 0, or -1 on a protocol violation
 */
static int mc_input(Conn *c, const uint8_t *data, size_t len, uint64_t now) {
  (void)data;
  c->in_len += len;
  size_t pos = 0;
  while (pos < c->in_len) {
    const uint8_t *p = c->in + pos, *end = c->in + c->in_len;
    const uint8_t *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) break;
    if (nl == p || nl[-1] != '\r') return proto_error(c, "memcache: malformed line");
    size_t line = (size_t)(nl + 1 - p), used = line;
    const uint8_t *text_end = nl - 1; /* at the '\r' */
    Pending req;
    int status;
    if (line >= 5 && p[0] == 'V' && p[1] == 'A' && p[2] == ' ') {
      const uint8_t *num_end = p + 3;
      while (num_end < text_end && *num_end != ' ') num_end++;
      long long vlen;
      if (parse_ll(p + 3, num_end, &vlen) < 0 || vlen < 0) return proto_error(c, "memcache: bad VA length");
      used = line + (size_t)vlen + 2;
      if ((size_t)(end - p) < used) {
        in_fit(c, used);
        break;
      }
      if (!fifo_pop(c, &req)) return proto_error(c, "memcache: unsolicited reply");
      status = req.op == OP_GET && value_ok(req.key, nl + 1, (size_t)vlen) ? ST_OK : ST_BAD_VALUE;
      complete(c, req.op, req.key, req.t0, now, status);
      pos += used;
      continue;
    }
    if (line == 4 && p[0] == 'E' && p[1] == 'N') {
      status = ST_MISS;
    } else if (line == 4 && ((p[0] == 'H' && p[1] == 'D') || (p[0] == 'M' && p[1] == 'N'))) {
      status = ST_OK;
    } else {
      if (!c->w->error_text[0])
        snprintf(c->w->error_text, sizeof c->w->error_text, "server error: %.*s", (int)(line - 2), p);
      status = ST_ERROR; /* NS/EX/NF, ERROR, CLIENT_ERROR, SERVER_ERROR */
    }
    if (!fifo_pop(c, &req)) return proto_error(c, "memcache: unsolicited reply");
    if (status == ST_OK && req.op == OP_GET) status = ST_BAD_VALUE;
    complete(c, req.op, req.key, req.t0, now, status);
    pos += used;
  }
  in_consume(c, pos);
  return 0;
}

const Proto proto_memcache = {
    .name = "memcache",
    .issue = mc_issue,
    .input = mc_input,
    .parses_in_place = 1,
};
