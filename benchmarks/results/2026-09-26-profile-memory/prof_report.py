#!/usr/bin/env python3
"""prof_report.py — group a flat perf report (dso,sym) of the server CPU into
hotspots and convert shares to ns per request.

Usage: prof_report.py PERF_TXT OPS_PER_SEC USER_TICKS SYS_TICKS [NM_TXT]
PERF_TXT is `perf report --no-children --sort dso,sym --stdio` output of a
system-wide cpu-clock recording restricted to the server's CPU. NM_TXT
(`nm -n build/kvasm`) lets NASM macro-local labels such as `..@67.done`
(which perf reports as symbols of their own) count toward the function that
contains them.
"""
import re
import sys

txt, ops, ut, st = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])

# Macro-local label -> enclosing function, from the address-sorted symbol
# table: the nearest preceding symbol that is not itself a local label.
owner = {}
if len(sys.argv) > 5:
    last_fn = None
    for line in open(sys.argv[5]):
        parts = line.split()
        if len(parts) != 3 or parts[1] not in 'tT':
            continue
        name = parts[2]
        if name.startswith('..@'):
            if last_fn:
                owner[name] = last_fn
        elif '.' not in name:
            last_fn = name

# KvAsm symbols by module, from the source tree's labels.
H2 = ('kv_h2n', 'h2_process', 'header_block', 'decode_fields', 'decode_discard', 'field_handler',
      'stream_open', 'stream_close', 'stream_error', 'map_find', 'map_insert', 'map_remove',
      'map_grow', 'reply_fast', 'reply_slow', 'put_response_headers', 'put_trailers', 'put_hdr',
      'put_tsu', 'put_nv_headers', 'encode_nv', 'put_string', 'write_frame', 'out_reserve',
      'emit_progress', 'hb_append', 'request_end', 'ring_has', 'pend_push', 'pend_unlink',
      'conn_window_refill', 'reset_budget', 'apply_initial_window', 'send_rst', 'send_goaway',
      'send_window_update', 'conn_error', 'blocks_equal')
HPACK = ('kv_hpack', 'kv_huff', 'hp_', 'classify')
GRPC = ('kv_grpc', 'dispatch', 'get_execute', 'stream_free', 'stream_unlink_free', 'write_prefix',
        'kv_pb_', 'pb_')
STORE = ('kv_store', 'kv_cuckoo', 'kv_siphash', 'kv_keyeq', 'kv_hcache', 'bucket_', 'scan_',
         'find_in', 'lookup', 'kv_qsbr')
LOOP = ('worker_main', 'conn_event', 'conn_flush', 'set_interest', 'housekeeping', 'accept_',
        'conn_open', 'conn_destroy', 'conn_linger', 'flush_or_close', 'epoll_add_in', 'now_ns')
POOL = ('kv_pool',)

groups = {}
detail = []
for line in open(txt, errors="replace"):
    m = re.match(r'\s*([\d.]+)%\s+(\S+)\s+\[(.)\]\s+(\S+)', line)
    if not m:
        continue
    pct, dso, space, sym = float(m.group(1)), m.group(2), m.group(3), m.group(4)
    if space == 'k':
        if sym.startswith(('tcp_', '__tcp', 'inet_', 'sk_', 'skb_', '__skb', 'ip_', '__ip',
                           'loopback', 'netif', '__netif', 'net_rx', '__dev', 'dev_', 'validate_xmit',
                           'process_backlog', 'sock_', '__sock', 'release_sock', 'lock_sock',
                           'napi', '__napi', 'kfree_skb', '__kfree_skb', 'raw_', 'nf_', 'ipv4',
                           'eth_', 'rps', 'bpf', 'security_sock', 'selinux', '__alloc_skb',
                           'kmalloc_reserve', 'skb')):
            g = 'kernel: TCP/IP loopback stack'
        elif sym.startswith(('copy_', '_copy', 'rep_movs', 'memcpy', '__memcpy', 'copyout',
                             'copyin', 'simple_copy', '__check_object', 'check_', 'csum')):
            g = 'kernel: user<->kernel copies'
        elif sym.startswith(('ep_', 'do_epoll', '__x64_sys_epoll', 'epoll', 'ep_poll')):
            g = 'kernel: epoll'
        elif sym.startswith(('entry_', 'do_syscall', 'syscall_', '__x64_sys', 'exit_to_user',
                             'syscall', '__fdget', 'fput', '__fget', 'ksys_', 'vfs_', 'rw_verify',
                             'sock_read_iter', 'sock_write_iter', 'new_sync', '__sys_', 'import_',
                             'fsnotify', 'security_file', 'apparmor', '__fsnotify')):
            g = 'kernel: syscall entry/exit, VFS'
        elif sym.startswith(('schedule', '__schedule', 'finish_task', 'pick_next', 'dequeue',
                             'enqueue', 'update_', 'try_to_wake', 'ttwu', 'hrtimer', 'native_',
                             'default_idle', 'do_idle', 'cpu_idle', 'irq', 'asm_', 'sysvec',
                             '__irq', 'tick', 'rcu', 'clockevents', 'ktime', 'wake', '__wake',
                             'autoremove', 'psi_', 'cpuacct', 'account', 'hv_', 'vmbus')):
            g = 'kernel: scheduler, IRQ, timers, idle'
        elif sym.startswith(('__slab', 'kmem_', 'kfree', '__kmalloc', 'slab', 'page_', '__page',
                             'alloc_', 'free_', '__free', 'get_page', 'mod_memcg', 'memcg',
                             'obj_cgroup', 'refill_', 'drain_', '__mod', 'try_charge')):
            g = 'kernel: memory allocation (slab, pages, memcg)'
        else:
            g = 'kernel: other'
    elif dso.endswith('kvasm'):
        sym = owner.get(sym, sym)
        if sym.startswith(HPACK):
            g = 'KvAsm: HPACK decode (hpack.asm)'
        elif sym.startswith(H2) or sym in H2:
            g = 'KvAsm: HTTP/2 framing (h2n.asm)'
        elif sym.startswith(GRPC) or sym in GRPC:
            g = 'KvAsm: gRPC dispatch + protobuf'
        elif sym.startswith(STORE):
            g = 'KvAsm: store lookup (hash, buckets, record)'
        elif sym.startswith(POOL):
            g = 'KvAsm: pool allocator'
        elif sym.startswith(LOOP) or sym in LOOP:
            g = 'KvAsm: event loop (worker.asm)'
        elif sym.startswith(('__mem', 'mem', '__libc', 'syscall', 'clock_gettime', '__clock',
                             'malloc', 'free', '_int_', 'realloc', 'calloc', 'pthread', '__GI',
                             '__vdso', 'epoll_wait', '__epoll', 'read', 'write', '__lll',
                             'aligned_alloc', '_dl_', '__strlen', 'strlen', '__memmove',
                             '__memset', '__memcmp', '__new_memcpy')):
            g = 'glibc (memcpy/memset/syscall stubs)'
        else:
            g = 'KvAsm: other (' + sym + ')'
    elif dso.startswith('[vdso]'):
        g = 'vDSO clock_gettime'
    else:
        g = 'other process on the CPU (' + dso + ')'
    groups[g] = groups.get(g, 0.0) + pct
    detail.append((pct, space, sym, g))

tot = sum(groups.values()) or 1.0
ns = 1e9 / ops
share_user = ut / max(ut + st, 1)
print(f"throughput {ops:,.0f} req/s on 1 core -> {ns:.0f} ns of server-core time per request")
print(f"/proc split: user {share_user * 100:.1f}% ({share_user * ns:.0f} ns), "
      f"kernel {100 - share_user * 100:.1f}% ({(1 - share_user) * ns:.0f} ns)")
print(f"sampled CPU time attributed (system-wide on the server's CPU): {tot:.1f}% of samples\n")
print(f"{'hotspot':55s} {'share':>7s} {'ns/req':>7s}")
for g, p in sorted(groups.items(), key=lambda kv: -kv[1]):
    print(f"{g:55s} {p / tot * 100:6.1f}% {p / tot * ns:7.0f}")
print("\ntop symbols:")
for pct, space, sym, g in sorted(detail, key=lambda d: -d[0])[:45]:
    print(f"  {pct:6.2f}%  [{space}] {sym:40s} {g}")
