# KvAsm re-measured (5b1161d) against 2026-09-27 (a0264f7)

Unpaired, across sessions: new ÷ old KvAsm with an exact Mann–Whitney p over the runs; KvAsm ÷ X compares this session's KvAsm median with the other store's median of the earlier session. Outside load = median logical CPUs busy outside the VM per run.

## E0

Outside load: this session 4.3, earlier session 16.7 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 cores, 1 threads | 971 k | 1.41 M | 0.69× (p=0.100) | 1.00 M | 0.97× | 1.26 M | 0.77× | 631 k | 1.54× | 747 k | 1.30× |
| 2 cores, 2 threads | 2.49 M | 3.08 M | 0.81× (p=0.100) | 1.38 M | 1.80× | 1.69 M | 1.47× | 1.07 M | 2.33× | 1.24 M | 2.01× |
| 4 cores, 3 threads | 4.05 M | 4.21 M | 0.96× (p=0.700) | 1.66 M | 2.44× | 2.19 M | 1.85× | 1.76 M | 2.30× | 2.16 M | 1.88× |
| 4 cores, 4 threads | 4.89 M | 5.02 M | 0.97× (p=0.700) | 1.71 M | 2.86× | 2.75 M | 1.77× | 2.03 M | 2.41× | 2.73 M | 1.79× |
| 8 cores, 7 threads | 5.43 M | 5.30 M | 1.02× (p=0.700) | 2.65 M | 2.05× | 2.03 M | 2.67× | 2.62 M | 2.07× | 4.13 M | 1.31× |
| 8 cores, 8 threads | 5.76 M | 4.62 M | 1.25× (p=0.100) | 1.83 M | 3.16× | 1.74 M | 3.30× | 2.23 M | 2.58× | 5.46 M | 1.06× |


## E1

Outside load: this session 4.1, earlier session 15.8 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 core | 989 k | 1.39 M | 0.71× (p=0.002) | 1.01 M | 0.98× | 1.04 M | 0.95× | 629 k | 1.57× | 642 k | 1.54× | 587 k | 1.68× |
| 2 cores | 2.47 M | 3.06 M | 0.81× (p=0.026) | 1.29 M | 1.91× | 1.68 M | 1.47× | 1.08 M | 2.29× | 1.51 M | 1.64× | 1.23 M | 2.00× |
| 4 cores | 4.92 M | 4.98 M | 0.99× (p=0.240) | 1.67 M | 2.94× | 2.46 M | 2.00× | 1.68 M | 2.93× | 3.32 M | 1.48× | 2.47 M | 1.99× |
| 8 cores | 5.75 M | 5.99 M | 0.96× (p=0.310) | 1.86 M | 3.10× | 1.82 M | 3.17× | 2.18 M | 2.64× | 4.96 M | 1.16× | 4.34 M | 1.33× |

**CPU ns/op**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 core | 1,007 | 720 | 1.40× (p=0.002) | 989 | 1.02× | 960 | 1.05× | 1,586 | 0.64× | 1,564 | 0.64× | 1,704 | 0.59× |
| 2 cores | 812 | 654 | 1.24× (p=0.026) | 1,175 | 0.69× | 1,195 | 0.68× | 1,850 | 0.44× | 1,308 | 0.62× | 1,604 | 0.51× |
| 4 cores | 755 | 753 | 1.00× (p=0.589) | 1,553 | 0.49× | 1,624 | 0.47× | 2,310 | 0.33× | 1,128 | 0.67× | 1,598 | 0.47× |
| 8 cores | 950 | 742 | 1.28× (p=0.026) | 2,052 | 0.46× | 3,663 | 0.26× | 3,098 | 0.31× | 1,302 | 0.73× | 1,819 | 0.52× |


## E2

Outside load: this session 4.4, earlier session 4.5 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 100% SET | 4.43 M | 2.23 M | 1.98× (p=0.002) | 2.58 M | 1.72× | 1.78 M | 2.49× | 2.48 M | 1.79× | 3.75 M | 1.18× | 1.54 M | 2.88× |
| 50% GET | 4.28 M | 3.79 M | 1.13× (p=0.002) | 2.65 M | 1.61× | 2.24 M | 1.91× | 2.33 M | 1.83× | 3.84 M | 1.11× | 2.36 M | 1.81× |
| 90% GET | 4.71 M | 7.66 M | 0.61× (p=0.002) | 2.88 M | 1.64× | 3.01 M | 1.56× | 2.43 M | 1.94× | 3.96 M | 1.19× | 2.95 M | 1.60× |
| 90% GET, Zipf | 4.76 M | 7.87 M | 0.61× (p=0.002) | 2.98 M | 1.60× | 3.09 M | 1.54× | 2.57 M | 1.85× | 4.19 M | 1.14× | 4.13 M | 1.15× |


## E3

Outside load: this session 4.5, earlier session 6.4 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 256×1 | 506 k | 815 k | 0.62× (p=0.002) | 393 k | 1.29× | 588 k | 0.86× | 440 k | 1.15× | 420 k | 1.20× | 609 k | 0.83× |

**p50 µs**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 256×1 | 452.6 | 274.4 | 1.65× (p=0.002) | 626.7 | 0.72× | 419.8 | 1.08× | 557.1 | 0.81× | 573.4 | 0.79× | 370.7 | 1.22× |

**p99 µs**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 256×1 | 1,212.4 | 790.5 | 1.53× (p=0.002) | 1,417.2 | 0.86× | 720.9 | 1.68× | 1,179.7 | 1.03× | 1,212.4 | 1.00× | 1,073.2 | 1.13× |


## E4

Outside load: this session 4.9, earlier session 5.8 (all systems).

**p50 µs**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1.25 M/s offered | 222.2 | 111.1 | 2.00× (p=0.100) | 378.9 | 0.59× | 173.1 | 1.28× | 553.0 | 0.40× | 134.1 | 1.66× | 134.1 | 1.66× |
| 418 k/s offered | 152.6 | 100.9 | 1.51× (p=0.100) | 222.2 | 0.69× | 92.7 | 1.65× | 171.0 | 0.89× | 107.0 | 1.43× | 96.8 | 1.58× |
| 836 k/s offered | 197.6 | 117.2 | 1.69× (p=0.100) | 317.4 | 0.62× | 142.3 | 1.39× | 387.1 | 0.51× | 134.1 | 1.47× | 107.0 | 1.85× |

**p99 µs**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1.25 M/s offered | 634.9 | 428.0 | 1.48× (p=0.100) | 913.4 | 0.70× | 395.3 | 1.61× | 2,342.9 | 0.27× | 92,799.0 | 0.01× | 1,089.5 | 0.58× |
| 418 k/s offered | 602.1 | 374.8 | 1.61× (p=0.100) | 651.3 | 0.92× | 252.9 | 2.38× | 847.9 | 0.71× | 1,269.8 | 0.47× | 428.0 | 1.41× |
| 836 k/s offered | 602.1 | 444.4 | 1.35× (p=0.100) | 888.8 | 0.68× | 358.4 | 1.68× | 1,515.5 | 0.40× | 8,978.4 | 0.07× | 634.9 | 0.95× |

**p99.9 µs**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1.25 M/s offered | 946.2 | 1,155.1 | 0.82× (p=1.000) | 1,286.1 | 0.74× | 643.1 | 1.47× | 3,391.5 | 0.28× | 274,726.9 | 0.00× | 2,506.8 | 0.38× |
| 418 k/s offered | 897.0 | 569.3 | 1.58× (p=0.100) | 954.4 | 0.94× | 448.5 | 2.00× | 1,646.6 | 0.54× | 13,434.9 | 0.07× | 667.6 | 1.34× |
| 836 k/s offered | 1,011.7 | 626.7 | 1.61× (p=0.700) | 1,515.5 | 0.67× | 700.4 | 1.44× | 2,506.8 | 0.40× | 35,389.4 | 0.03× | 1,105.9 | 0.91× |


## E5

Outside load: this session 3.6, earlier session 3.6 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| no-op | 1.21 M | 2.13 M | 0.57× (p=0.002) | 2.11 M | 0.57× | 1.93 M | 0.63× | 1.86 M | 0.65× | 1.30 M | 0.93× | 550 k | 2.19× |

**CPU ns/op**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| no-op | 835 | 472 | 1.77× (p=0.002) | 479 | 1.74× | 525 | 1.59× | 541 | 1.54× | 774 | 1.08× | 1,837 | 0.45× |


## E6

Outside load: this session 5.2, earlier session 4.3 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1024 B values | 3.84 M | 5.59 M | 0.69× (p=0.100) | 1.91 M | 2.01× | 1.85 M | 2.07× | 1.53 M | 2.51× | 3.16 M | 1.22× | 2.47 M | 1.55× |
| 16384 B values | 646 k | 481 k | 1.34× (p=0.100) | 461 k | 1.40× | 273 k | 2.36× | 444 k | 1.46× | 972 k | 0.67× | 995 k | 0.65× |


## E7

Outside load: this session 6.6, earlier session 4.4 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 16×1 | 164 k | 314 k | 0.52× (p=0.100) | 175 k | 0.94× | 275 k | 0.60× | 235 k | 0.70× | 250 k | 0.66× | 347 k | 0.47× |
| 1×1 | 19 k | 35 k | 0.53× (p=0.100) | 19 k | 0.97× | 35 k | 0.54× | 21 k | 0.89× | 35 k | 0.54× | 37 k | 0.51× |
| 64×1 | 405 k | 714 k | 0.57× (p=0.100) | 297 k | 1.36× | 426 k | 0.95× | 423 k | 0.96× | 432 k | 0.94× | 519 k | 0.78× |
| 64×16 | 4.90 M | 9.11 M | 0.54× (p=0.100) | 2.91 M | 1.68× | 3.42 M | 1.43× | 2.50 M | 1.96× | 4.09 M | 1.20× | 3.15 M | 1.56× |
| 64×4 | 1.38 M | 2.94 M | 0.47× (p=0.100) | 1.05 M | 1.31× | 1.56 M | 0.89× | 1.07 M | 1.29× | 1.50 M | 0.92× | 1.50 M | 0.92× |

**p99 µs**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 16×1 | 264.2 | 121.3 | 2.18× (p=0.100) | 220.2 | 1.20× | 95.7 | 2.76× | 189.4 | 1.39× | 179.2 | 1.47× | 148.5 | 1.78× |
| 1×1 | 97.8 | 67.1 | 1.46× (p=0.100) | 102.9 | 0.95× | 69.1 | 1.41× | 107.0 | 0.91× | 79.4 | 1.23× | 62.7 | 1.56× |
| 64×1 | 415.7 | 230.4 | 1.80× (p=0.100) | 423.9 | 0.98× | 232.4 | 1.79× | 325.6 | 1.28× | 301.1 | 1.38× | 342.0 | 1.22× |
| 64×16 | 643.1 | 305.1 | 2.11× (p=0.100) | 708.6 | 0.91× | 477.2 | 1.35× | 741.4 | 0.87× | 423.9 | 1.52× | 651.3 | 0.99× |
| 64×4 | 460.8 | 222.2 | 2.07× (p=0.100) | 456.7 | 1.01× | 276.5 | 1.67× | 456.7 | 1.01× | 309.2 | 1.49× | 864.3 | 0.53× |


## E8

Outside load: this session —, earlier session — (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis |
|---|---|---|---|---|---|
| h2load-onekey | 3.25 M | 5.89 M | 0.55× (p=0.100) | — | — |
| kvbench-onekey | 5.11 M | 9.30 M | 0.55× (p=0.100) | 3.43 M | 1.49× |


## N1

Outside load: this session 3.7, earlier session 4.6 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 1.01 M | 1.76 M | 0.58× (p=0.002) | 1.40 M | 0.72× | 1.42 M | 0.71× |

**CPU ns/op**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 993 | 576 | 1.72× (p=0.002) | 722 | 1.37× | 710 | 1.40× |

**p50 µs**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 958.5 | 532.5 | 1.80× (p=0.002) | 704.5 | 1.36× | 696.3 | 1.38× |

**p99 µs**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 2,015.2 | 1,171.5 | 1.72× (p=0.002) | 1,278.0 | 1.58× | 1,212.4 | 1.66× |


## N3

Outside load: this session 3.6, earlier session 4.6 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 1.13 M | 2.13 M | 0.53× (p=0.002) | 2.06 M | 0.55× | 1.93 M | 0.58× |

**CPU ns/op**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey |
|---|---|---|---|---|---|---|---|
| 1 core | 887 | 475 | 1.87× (p=0.002) | 491 | 1.80× | 526 | 1.69× |


## S1

Outside load: this session 4.8, earlier session 5.2 (all systems).

**ops/s**

| cell | KvAsm now | KvAsm before | now ÷ before (MW p) | redis | KvAsm ÷ redis | valkey | KvAsm ÷ valkey | dragonfly | KvAsm ÷ dragonfly | garnet | KvAsm ÷ garnet | memcached | KvAsm ÷ memcached |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 cores | 4.84 M | 8.84 M | 0.55× | 2.88 M | 1.68× | 3.20 M | 1.51× | 2.40 M | 2.01× | 3.86 M | 1.25× | 3.01 M | 1.61× |

For ops/s, ratios above 1 favor the first-named; for latency and CPU, below 1 does.
