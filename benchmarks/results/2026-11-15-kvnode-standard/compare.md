# KvNode re-measured (0e48073) against 2026-09-27 (a0264f7)

Unpaired, across sessions: new ÷ old KvNode with an exact Mann–Whitney p over the runs; KvNode ÷ X compares this session's KvNode median with the other store's median of the earlier session. Outside load = median logical CPUs busy outside the VM per run.

## E0

Outside load: this session 4.9, earlier session 16.7 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 cores, 1 threads | 955 k | 1.41 M | 0.68× (p=0.100) | 1.00 M | 0.95× | 1.26 M | 0.76× | 631 k | 1.51× | 747 k | 1.28× |
| 2 cores, 2 threads | 2.35 M | 3.08 M | 0.76× (p=0.100) | 1.38 M | 1.70× | 1.69 M | 1.39× | 1.07 M | 2.20× | 1.24 M | 1.90× |
| 4 cores, 3 threads | 3.87 M | 4.21 M | 0.92× (p=0.200) | 1.66 M | 2.33× | 2.19 M | 1.77× | 1.76 M | 2.20× | 2.16 M | 1.79× |
| 4 cores, 4 threads | 4.76 M | 5.02 M | 0.95× (p=0.400) | 1.71 M | 2.78× | 2.75 M | 1.73× | 2.03 M | 2.35× | 2.73 M | 1.74× |
| 8 cores, 7 threads | 5.30 M | 5.30 M | 1.00× (p=1.000) | 2.65 M | 2.00× | 2.03 M | 2.61× | 2.62 M | 2.02× | 4.13 M | 1.28× |
| 8 cores, 8 threads | 5.29 M | 4.62 M | 1.14× (p=0.400) | 1.83 M | 2.90× | 1.74 M | 3.03× | 2.23 M | 2.37× | 5.46 M | 0.97× |


## E1

Outside load: this session 5.8, earlier session 15.8 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 core | 919 k | 1.39 M | 0.66× (p=0.002) | 1.01 M | 0.91× | 1.04 M | 0.88× | 629 k | 1.46× | 642 k | 1.43× | 587 k | 1.57× |
| 2 cores | 2.30 M | 3.06 M | 0.75× (p=0.002) | 1.29 M | 1.78× | 1.68 M | 1.37× | 1.08 M | 2.14× | 1.51 M | 1.53× | 1.23 M | 1.87× |
| 4 cores | 4.47 M | 4.98 M | 0.90× (p=0.002) | 1.67 M | 2.67× | 2.46 M | 1.82× | 1.68 M | 2.66× | 3.32 M | 1.34× | 2.47 M | 1.81× |
| 8 cores | 5.27 M | 5.99 M | 0.88× (p=0.041) | 1.86 M | 2.84× | 1.82 M | 2.90× | 2.18 M | 2.42× | 4.96 M | 1.06× | 4.34 M | 1.22× |

**CPU ns/op**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 core | 1,086 | 720 | 1.51× (p=0.002) | 989 | 1.10× | 960 | 1.13× | 1,586 | 0.68× | 1,564 | 0.69× | 1,704 | 0.64× |
| 2 cores | 869 | 654 | 1.33× (p=0.002) | 1,175 | 0.74× | 1,195 | 0.73× | 1,850 | 0.47× | 1,308 | 0.66× | 1,604 | 0.54× |
| 4 cores | 866 | 753 | 1.15× (p=0.002) | 1,553 | 0.56× | 1,624 | 0.53× | 2,310 | 0.37× | 1,128 | 0.77× | 1,598 | 0.54× |
| 8 cores | 1,060 | 742 | 1.43× (p=0.002) | 2,052 | 0.52× | 3,663 | 0.29× | 3,098 | 0.34× | 1,302 | 0.81× | 1,819 | 0.58× |


## E2

Outside load: this session 6.9, earlier session 4.5 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 100% SET | 4.00 M | 2.23 M | 1.79× (p=0.002) | 2.58 M | 1.55× | 1.78 M | 2.25× | 2.48 M | 1.61× | 3.75 M | 1.07× | 1.54 M | 2.60× |
| 50% GET | 3.73 M | 3.79 M | 0.99× (p=0.589) | 2.65 M | 1.41× | 2.24 M | 1.66× | 2.33 M | 1.60× | 3.84 M | 0.97× | 2.36 M | 1.58× |
| 90% GET | 4.02 M | 7.66 M | 0.53× (p=0.002) | 2.88 M | 1.40× | 3.01 M | 1.34× | 2.43 M | 1.66× | 3.96 M | 1.02× | 2.95 M | 1.37× |
| 90% GET, Zipf | 4.18 M | 7.87 M | 0.53× (p=0.002) | 2.98 M | 1.40× | 3.09 M | 1.35× | 2.57 M | 1.63× | 4.19 M | 1.00× | 4.13 M | 1.01× |


## E3

Outside load: this session 7.9, earlier session 6.4 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 256×1 | 447 k | 815 k | 0.55× (p=0.002) | 393 k | 1.14× | 588 k | 0.76× | 440 k | 1.02× | 420 k | 1.06× | 609 k | 0.73× |

**p50 µs**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 256×1 | 518.1 | 274.4 | 1.89× (p=0.002) | 626.7 | 0.83× | 419.8 | 1.23× | 557.1 | 0.93× | 573.4 | 0.90× | 370.7 | 1.40× |

**p99 µs**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 256×1 | 1,409.0 | 790.5 | 1.78× (p=0.002) | 1,417.2 | 0.99× | 720.9 | 1.95× | 1,179.7 | 1.19× | 1,212.4 | 1.16× | 1,073.2 | 1.31× |


## E4

Outside load: this session 6.2, earlier session 5.8 (all systems).

**p50 µs**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1.25 M/s offered | 242.7 | 111.1 | 2.18× (p=0.100) | 378.9 | 0.64× | 173.1 | 1.40× | 553.0 | 0.44× | 134.1 | 1.81× | 134.1 | 1.81× |
| 418 k/s offered | 189.4 | 100.9 | 1.88× (p=0.100) | 222.2 | 0.85× | 92.7 | 2.04× | 171.0 | 1.11× | 107.0 | 1.77× | 96.8 | 1.96× |
| 836 k/s offered | 209.9 | 117.2 | 1.79× (p=0.100) | 317.4 | 0.66× | 142.3 | 1.47× | 387.1 | 0.54× | 134.1 | 1.56× | 107.0 | 1.96× |

**p99 µs**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1.25 M/s offered | 708.6 | 428.0 | 1.66× (p=0.100) | 913.4 | 0.78× | 395.3 | 1.79× | 2,342.9 | 0.30× | 92,799.0 | 0.01× | 1,089.5 | 0.65× |
| 418 k/s offered | 766.0 | 374.8 | 2.04× (p=0.100) | 651.3 | 1.18× | 252.9 | 3.03× | 847.9 | 0.90× | 1,269.8 | 0.60× | 428.0 | 1.79× |
| 836 k/s offered | 700.4 | 444.4 | 1.58× (p=0.100) | 888.8 | 0.79× | 358.4 | 1.95× | 1,515.5 | 0.46× | 8,978.4 | 0.08× | 634.9 | 1.10× |

**p99.9 µs**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1.25 M/s offered | 1,237.0 | 1,155.1 | 1.07× (p=0.700) | 1,286.1 | 0.96× | 643.1 | 1.92× | 3,391.5 | 0.36× | 274,726.9 | 0.00× | 2,506.8 | 0.49× |
| 418 k/s offered | 1,155.1 | 569.3 | 2.03× (p=0.100) | 954.4 | 1.21× | 448.5 | 2.58× | 1,646.6 | 0.70× | 13,434.9 | 0.09× | 667.6 | 1.73× |
| 836 k/s offered | 978.9 | 626.7 | 1.56× (p=0.700) | 1,515.5 | 0.65× | 700.4 | 1.40× | 2,506.8 | 0.39× | 35,389.4 | 0.03× | 1,105.9 | 0.89× |


## E5

Outside load: this session 4.8, earlier session 3.6 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| no-op | 1.12 M | 2.13 M | 0.53× (p=0.002) | 2.11 M | 0.53× | 1.93 M | 0.58× | 1.86 M | 0.60× | 1.30 M | 0.86× | 550 k | 2.04× |

**CPU ns/op**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| no-op | 894 | 472 | 1.89× (p=0.002) | 479 | 1.87× | 525 | 1.70× | 541 | 1.65× | 774 | 1.15× | 1,837 | 0.49× |


## E6

Outside load: this session 4.4, earlier session 4.3 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1024 B values | 3.23 M | 5.59 M | 0.58× (p=0.100) | 1.91 M | 1.69× | 1.85 M | 1.74× | 1.53 M | 2.11× | 3.16 M | 1.02× | 2.47 M | 1.31× |
| 16384 B values | 317 k | 481 k | 0.66× (p=0.100) | 461 k | 0.69× | 273 k | 1.16× | 444 k | 0.72× | 972 k | 0.33× | 995 k | 0.32× |


## E7

Outside load: this session 6.4, earlier session 4.4 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 16×1 | 149 k | 314 k | 0.48× (p=0.100) | 175 k | 0.85× | 275 k | 0.54× | 235 k | 0.63× | 250 k | 0.60× | 347 k | 0.43× |
| 1×1 | 15 k | 35 k | 0.42× (p=0.100) | 19 k | 0.77× | 35 k | 0.42× | 21 k | 0.70× | 35 k | 0.43× | 37 k | 0.40× |
| 64×1 | 370 k | 714 k | 0.52× (p=0.100) | 297 k | 1.25× | 426 k | 0.87× | 423 k | 0.87× | 432 k | 0.86× | 519 k | 0.71× |
| 64×16 | 4.17 M | 9.11 M | 0.46× (p=0.100) | 2.91 M | 1.43× | 3.42 M | 1.22× | 2.50 M | 1.67× | 4.09 M | 1.02× | 3.15 M | 1.32× |
| 64×4 | 1.39 M | 2.94 M | 0.47× (p=0.100) | 1.05 M | 1.31× | 1.56 M | 0.89× | 1.07 M | 1.30× | 1.50 M | 0.93× | 1.50 M | 0.92× |

**p99 µs**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 16×1 | 284.7 | 121.3 | 2.35× (p=0.100) | 220.2 | 1.29× | 95.7 | 2.97× | 189.4 | 1.50× | 179.2 | 1.59× | 148.5 | 1.92× |
| 1×1 | 171.0 | 67.1 | 2.55× (p=0.100) | 102.9 | 1.66× | 69.1 | 2.47× | 107.0 | 1.60× | 79.4 | 2.15× | 62.7 | 2.73× |
| 64×1 | 473.1 | 230.4 | 2.05× (p=0.100) | 423.9 | 1.12× | 232.4 | 2.04× | 325.6 | 1.45× | 301.1 | 1.57× | 342.0 | 1.38× |
| 64×16 | 643.1 | 305.1 | 2.11× (p=0.100) | 708.6 | 0.91× | 477.2 | 1.35× | 741.4 | 0.87× | 423.9 | 1.52× | 651.3 | 0.99× |
| 64×4 | 514.0 | 222.2 | 2.31× (p=0.100) | 456.7 | 1.13× | 276.5 | 1.86× | 456.7 | 1.13× | 309.2 | 1.66× | 864.3 | 0.59× |


## E8

Outside load: this session —, earlier session — (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis |
|---|---|---|---|---|---|
| h2load-onekey | 3.20 M | 5.89 M | 0.54× (p=0.100) | — | — |
| kvbench-onekey | 4.92 M | 9.30 M | 0.53× (p=0.100) | 3.43 M | 1.44× |


## N1

Outside load: this session 5.2, earlier session 4.6 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 909 k | 1.76 M | 0.52× (p=0.002) | 1.40 M | 0.65× | 1.42 M | 0.64× |

**CPU ns/op**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 1,106 | 576 | 1.92× (p=0.002) | 722 | 1.53× | 710 | 1.56× |

**p50 µs**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 1,034.2 | 532.5 | 1.94× (p=0.002) | 704.5 | 1.47× | 696.3 | 1.49× |

**p99 µs**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 2,195.5 | 1,171.5 | 1.87× (p=0.002) | 1,278.0 | 1.72× | 1,212.4 | 1.81× |


## N3

Outside load: this session 4.0, earlier session 4.6 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 1.10 M | 2.13 M | 0.51× (p=0.002) | 2.06 M | 0.53× | 1.93 M | 0.57× |

**CPU ns/op**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 913 | 475 | 1.92× (p=0.002) | 491 | 1.86× | 526 | 1.74× |


## S1

Outside load: this session 4.8, earlier session 5.2 (all systems).

**ops/s**

| cell | KvNode now | KvNode before | now ÷ before (MW p) | redis | KvNode ÷ redis | valkey | KvNode ÷ valkey | dragonfly | KvNode ÷ dragonfly | garnet | KvNode ÷ garnet | memcached | KvNode ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 cores | 4.76 M | 8.84 M | 0.54× | 2.88 M | 1.66× | 3.20 M | 1.49× | 2.40 M | 1.98× | 3.86 M | 1.23× | 3.01 M | 1.58× |

For ops/s, ratios above 1 favor the first-named; for latency and CPU, below 1 does.
