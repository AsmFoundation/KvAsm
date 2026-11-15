# KvNode re-measured (c1b0f67) against 2026-09-27 (a0264f7)

Unpaired, across sessions: new ÷ old KvNode with an exact Mann–Whitney p over the runs; KvNode ÷ X compares this session's KvNode median with the other store's median of the earlier session. Outside load = median logical CPUs busy outside the VM per run.

## E0

Outside load: this session 6.7, earlier session 16.7 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 cores, 1 threads | 936 k | 1.41 M | 0.67× (p=0.100) | 1.00 M | 0.94× | 1.26 M | 0.75× | 631 k | 1.48× | 747 k | 1.25× |
| 2 cores, 2 threads | 2.22 M | 3.08 M | 0.72× (p=0.100) | 1.38 M | 1.61× | 1.69 M | 1.31× | 1.07 M | 2.07× | 1.24 M | 1.79× |
| 4 cores, 3 threads | 3.74 M | 4.21 M | 0.89× (p=0.100) | 1.66 M | 2.25× | 2.19 M | 1.71× | 1.76 M | 2.12× | 2.16 M | 1.73× |
| 4 cores, 4 threads | 4.45 M | 5.02 M | 0.89× (p=0.400) | 1.71 M | 2.60× | 2.75 M | 1.61× | 2.03 M | 2.19× | 2.73 M | 1.63× |
| 8 cores, 7 threads | 5.05 M | 5.30 M | 0.95× (p=0.700) | 2.65 M | 1.91× | 2.03 M | 2.49× | 2.62 M | 1.93× | 4.13 M | 1.22× |
| 8 cores, 8 threads | 5.33 M | 4.62 M | 1.15× (p=1.000) | 1.83 M | 2.92× | 1.74 M | 3.06× | 2.23 M | 2.39× | 5.46 M | 0.98× |


## E1

Outside load: this session 8.1, earlier session 15.8 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 core | 918 k | 1.39 M | 0.66× (p=0.002) | 1.01 M | 0.91× | 1.04 M | 0.88× | 629 k | 1.46× | 642 k | 1.43× | 587 k | 1.56× |
| 2 cores | 2.05 M | 3.06 M | 0.67× (p=0.002) | 1.29 M | 1.59× | 1.68 M | 1.22× | 1.08 M | 1.91× | 1.51 M | 1.36× | 1.23 M | 1.66× |
| 4 cores | 4.49 M | 4.98 M | 0.90× (p=0.002) | 1.67 M | 2.69× | 2.46 M | 1.83× | 1.68 M | 2.68× | 3.32 M | 1.35× | 2.47 M | 1.82× |
| 8 cores | 5.25 M | 5.99 M | 0.88× (p=0.065) | 1.86 M | 2.83× | 1.82 M | 2.89× | 2.18 M | 2.41× | 4.96 M | 1.06× | 4.34 M | 1.21× |

**CPU ns/op**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 core | 1,084 | 720 | 1.51× (p=0.002) | 989 | 1.10× | 960 | 1.13× | 1,586 | 0.68× | 1,564 | 0.69× | 1,704 | 0.64× |
| 2 cores | 972 | 654 | 1.49× (p=0.002) | 1,175 | 0.83× | 1,195 | 0.81× | 1,850 | 0.53× | 1,308 | 0.74× | 1,604 | 0.61× |
| 4 cores | 853 | 753 | 1.13× (p=0.002) | 1,553 | 0.55× | 1,624 | 0.53× | 2,310 | 0.37× | 1,128 | 0.76× | 1,598 | 0.53× |
| 8 cores | 1,049 | 742 | 1.41× (p=0.002) | 2,052 | 0.51× | 3,663 | 0.29× | 3,098 | 0.34× | 1,302 | 0.81× | 1,819 | 0.58× |


## E2

Outside load: this session 6.0, earlier session 4.5 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 100% SET | 4.00 M | 2.23 M | 1.79× (p=0.286) | 2.58 M | 1.55× | 1.78 M | 2.25× | 2.48 M | 1.61× | 3.75 M | 1.07× | 1.54 M | 2.60× |
| 50% GET | 3.81 M | 3.79 M | 1.01× (p=0.857) | 2.65 M | 1.44× | 2.24 M | 1.70× | 2.33 M | 1.63× | 3.84 M | 0.99× | 2.36 M | 1.61× |
| 90% GET, Zipf | 4.48 M | 7.87 M | 0.57× (p=0.286) | 2.98 M | 1.50× | 3.09 M | 1.45× | 2.57 M | 1.74× | 4.19 M | 1.07× | 4.13 M | 1.08× |

For ops/s, ratios above 1 favor the first-named; for latency and CPU, below 1 does.
