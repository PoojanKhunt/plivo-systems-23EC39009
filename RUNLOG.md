# RUNLOG — Systems Engineering Handout

## Overview
This log documents the iterative engineering and parametric tuning of a custom real-time UDP audio transport protocol over simulated hostile network environments (Profile A and Profile B). All tests were executed against a 1500-frame (30-second) byte-exact judging stream.

---

## Phase 1: Baseline & Simple Piggybacking (Profile A Tuning)

### Experiment 1.1: Naive 1-Frame Piggybacking at 40 ms
* **Profile:** `profiles/A.json`
* **DELAY_MS:** `40.0`
* **Deadline Misses:** 70 frames (4.67%) — **INVALID** (Cap: 1.00%)
* **Bandwidth Overhead:** 1.82x (Up: 438,000B) — **VALID** (Cap: 2.00x)
* **What Changed:** Implemented basic Forward Error Correction (FEC) in `sender.c` by attaching the previous payload ($i-1$) to the current packet ($i$) on 4 out of every 5 packets (`seq % 5 != 0`).
* **Why:** To recover from isolated single-packet network drops without relying on latency-prohibitive NACK retransmissions.
* **Analysis:** While overhead remained safely under 2.00x, a 40 ms playout buffer was too tight for Profile A's natural relay latency fluctuations (~20–40 ms), causing late-arriving packets to miss the playback clock.

### Experiment 1.2: Relaxing Playout Window to 60 ms
* **Profile:** `profiles/A.json`
* **DELAY_MS:** `60.0`
* **Deadline Misses:** 8 frames (0.53%) — **VALID**
* **Bandwidth Overhead:** 1.82x — **VALID**
* **What Changed:** Increased the receiver's playout delay parameter from 40 ms to 60 ms without modifying the wire protocol.
* **Why:** To test the latency floor of Profile A and determine if deadline misses were caused by physical packet loss or buffer underflow.
* **Analysis:** Miss rate dropped by over 4%, confirming the network was delivering packets safely but required at least ~50 ms of jitter buffering to prevent timing underflows.

### Experiment 1.3: Parametric Floor Optimization for Profile A
* **Profile:** `profiles/A.json`
* **DELAY_MS:** `50.0`
* **Deadline Misses:** 14 frames (0.93%) — **VALID**
* **Bandwidth Overhead:** 1.82x — **VALID**
* **What Changed:** Reduced playout delay step-by-step (60 ms $\rightarrow$ 50 ms $\rightarrow$ 48 ms $\rightarrow$ 45 ms).
* **Why:** The evaluation rubric penalizes excess playout delay. The goal was to identify the absolute lowest delay that satisfies the $\le 1.00\%$ miss cap.
* **Analysis:** At 48 ms and 45 ms, misses rose to 1.13% and 1.27% respectively. `50.0 ms` established the initial baseline floor for Profile A.

---

## Phase 2: The Profile B Burst-Loss Crisis & Sliding FEC

### Experiment 2.1: Applying Profile A Baseline to Profile B
* **Profile:** `profiles/B.json`
* **DELAY_MS:** `50.0`
* **Deadline Misses:** 748 frames (49.87%) — **INVALID**
* **Bandwidth Overhead:** 1.82x (81 dropped packets, 17 duplicated)
* **What Changed:** Tested the existing 1-frame piggybacking protocol against the high-jitter, burst-loss environment of Profile B.
* **Why:** To evaluate protocol generalization across hidden/harsh network profiles.
* **Analysis:** Catastrophic failure. Profile B introduces severe transit jitter (80–120 ms) and multi-packet burst drops. Simple 1-frame piggybacking completely fails when consecutive packets ($i$ and $i+1$) are dropped simultaneously.

### Experiment 2.2: Jitter Floor Discovery on Profile B
* **Profile:** `profiles/B.json`
* **DELAY_MS:** `100.0`
* **Deadline Misses:** 36 frames (2.40%) — **INVALID**
* **Bandwidth Overhead:** 1.86x (Up: 446,000B)
* **What Changed:** Increased `--delay_ms` from 50 ms to 100 ms and updated `sender.c` to use a 6-packet sliding FEC schedule alternating between ($i-1$) and ($i-2$) backups.
* **Why:** To separate latency misses from burst-loss misses.
* **Analysis:** Misses dropped from 50% to 2.40%. This proved that Profile B requires a ~100 ms buffer window to absorb network delay spikes, but ~36 frames were still being lost due to "schedule holes" where certain triads received no backup.

---

## Phase 3: Lock-Free Atomics & Overlapping XOR Parity

### Experiment 3.1: RFC 5109 Overlapping Triad XOR Parity
* **Profile:** `profiles/B.json`
* **DELAY_MS:** `100.0`
* **Deadline Misses:** 27 frames (1.80%) — **INVALID**
* **Bandwidth Overhead:** 1.86x (Up: 445,335B)
* **What Changed:** Redesigned the wire format into explicit Data Packets (`type = 0`) and Overlapping XOR Parity Packets (`type = 1`). Parity was computed across sliding triads ($P = F_{i-2} \oplus F_{i-1} \oplus F_i$) on 80% of frames (`seq % 5 != 0`).
* **Why:** To eliminate schedule holes and enable zero-RTT algebraic reconstruction of multi-packet burst losses without exceeding the 2.00x bandwidth budget.
* **Analysis:** Recovery improved significantly (reconstructing over 120 of the 149 dropped packets), but the receiver still missed 27 frames right at the playout boundary.

### Experiment 3.2: Lock-Free C11 Atomics & Two-Stage Busy-Spinning
* **Profile:** `profiles/B.json`
* **DELAY_MS:** `110.0`
* **Deadline Misses:** 10 frames (0.67%) — **VALID**
* **Bandwidth Overhead:** 1.86x — **VALID**
* **What Changed:** Replaced all `pthread_mutex_t` locks in `receiver.c` with C11 `<stdatomic.h>` lock-free memory orders (`memory_order_release` / `memory_order_acquire`). Upgraded the playout timing loop from `usleep(50)` to a two-stage model: sleep until `deadline - 2ms`, then lock-free spin-wait until exactly `20 microseconds` before playout.
* **Why:** To eliminate Linux OS scheduler wake-up latency and thread contention between the UDP receive loop and the audio dispatch timer.
* **Analysis:** Successful validation. Eliminating scheduler latency and locking overhead allowed the XOR recovery engine to reconstruct late-arriving packets in memory microseconds before audio dispatch.

---

## Phase 4: Final Parametric Optimization (The Winning Runs)

### Experiment 4.1: Final Profile A Floor Certification
* **Profile:** `profiles/A.json`
* **DELAY_MS:** `63.0`
* **Deadline Misses:** 13 frames (0.87%) — **VALID**
* **Bandwidth Overhead:** 1.86x (Up: 445,335B, 63 dropped by relay)
* **What Changed:** Tested the lock-free overlapping XOR protocol on Profile A while tuning down the delay window from 65 ms to 63 ms.
* **Why:** To lock in the minimum valid latency score for Profile A.
* **Verdict:** **VALID FINAL SCORE FOR PROFILE A: 63.0 ms**

### Experiment 4.2: Final Profile B Floor Certification
* **Profile:** `profiles/B.json`
* **DELAY_MS:** `107.0`
* **Deadline Misses:** 14 frames (0.93%) — **VALID**
* **Bandwidth Overhead:** 1.86x (Up: 445,335B, 149 dropped by relay)
* **What Changed:** Tested the lock-free overlapping XOR protocol on Profile B while fine-tuning the delay window down from 110 ms to 107 ms.
* **Why:** To lock in the minimum valid latency score for Profile B without crossing the 1.00% miss threshold.
* **Verdict:** **VALID FINAL SCORE FOR PROFILE B: 107.0 ms**

---

## Summary of Graded Parameters
| Profile | Optimal Delay | Miss Rate | Overhead | Final Status |
| :--- | :--- | :--- | :--- | :--- |
| **Profile A** | **63.0 ms** | **0.87% (13/1500)** | **1.86x** | **VALID** |
| **Profile B** | **107.0 ms** | **0.93% (14/1500)** | **1.86x** | **VALID** |