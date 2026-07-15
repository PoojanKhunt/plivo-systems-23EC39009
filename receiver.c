#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define IN_PORT 47002
#define OUT_PORT 47020
#define PAYLOAD_SIZE 160
#define RAW_FRAME_SIZE 164
#define BUF_SIZE 65536

typedef struct {
  uint8_t payload[PAYLOAD_SIZE];
  atomic_int received;
} Frame;

typedef struct {
  uint32_t base_seq;
  uint8_t parity[PAYLOAD_SIZE];
  atomic_int received;
} ParityBlock;

Frame buffer[BUF_SIZE];
ParityBlock parity_buffer[BUF_SIZE];

double get_now() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Zero-RTT Lock-Free XOR Recovery Engine
void attempt_recovery(uint32_t target_seq) {
  for (int offset = 0; offset <= 2; offset++) {
    if (target_seq < (uint32_t)offset)
      continue;
    uint32_t base = target_seq - offset;
    ParityBlock *pb = &parity_buffer[base % BUF_SIZE];

    if (atomic_load_explicit(&pb->received, memory_order_acquire) == 0 ||
        pb->base_seq != base)
      continue;

    int missing_count = 0;
    uint32_t missing_seq = 0;
    for (int i = 0; i < 3; i++) {
      uint32_t s = base + i;
      if (atomic_load_explicit(&buffer[s % BUF_SIZE].received,
                               memory_order_acquire) == 0) {
        missing_count++;
        missing_seq = s;
      }
    }

    // If target_seq is the sole missing frame in this overlapping triad,
    // reconstruct instantly!
    if (missing_count == 1 && missing_seq == target_seq) {
      uint8_t rec[PAYLOAD_SIZE];
      memcpy(rec, pb->parity, PAYLOAD_SIZE);
      for (int i = 0; i < 3; i++) {
        uint32_t s = base + i;
        if (s != target_seq) {
          for (int j = 0; j < PAYLOAD_SIZE; j++) {
            rec[j] ^= buffer[s % BUF_SIZE].payload[j];
          }
        }
      }
      memcpy(buffer[target_seq % BUF_SIZE].payload, rec, PAYLOAD_SIZE);

      atomic_store_explicit(&buffer[target_seq % BUF_SIZE].received, 1,
                            memory_order_release);
      return;
    }
  }
}

void *receive_loop(void *arg) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr_in;
  memset(&addr_in, 0, sizeof(addr_in));
  addr_in.sin_family = AF_INET;
  addr_in.sin_port = htons(IN_PORT);
  addr_in.sin_addr.s_addr = inet_addr("127.0.0.1");

  if (bind(sock, (struct sockaddr *)&addr_in, sizeof(addr_in)) < 0) {
    perror("Receiver bind failed on 47002");
    exit(1);
  }

  uint8_t wire_buf[512];

  while (1) {
    ssize_t n = recv(sock, wire_buf, sizeof(wire_buf), 0);
    if (n <= 0)
      continue;

    uint8_t pkt_type = wire_buf[0];

    if (pkt_type == 0 && n >= 165) {
      // Type 0: Data Packet
      uint32_t net_seq;
      memcpy(&net_seq, wire_buf + 1, 4);
      uint32_t seq = ntohl(net_seq);

      uint32_t idx = seq % BUF_SIZE;
      if (atomic_load_explicit(&buffer[idx].received, memory_order_acquire) ==
          0) {
        memcpy(buffer[idx].payload, wire_buf + 5, PAYLOAD_SIZE);
        atomic_store_explicit(&buffer[idx].received, 1, memory_order_release);
        // Trigger non-blocking neighbor recovery
        if (seq > 0)
          attempt_recovery(seq - 1);
        attempt_recovery(seq + 1);
      }
    } else if (pkt_type == 1 && n >= 165) {
      // Type 1: Overlapping Parity Packet
      uint32_t net_base;
      memcpy(&net_base, wire_buf + 1, 4);
      uint32_t base = ntohl(net_base);

      uint32_t p_idx = base % BUF_SIZE;
      parity_buffer[p_idx].base_seq = base;
      memcpy(parity_buffer[p_idx].parity, wire_buf + 5, PAYLOAD_SIZE);
      atomic_store_explicit(&parity_buffer[p_idx].received, 1,
                            memory_order_release);

      // Instantly attempt XOR recovery across all three frames covered by this
      // window
      attempt_recovery(base);
      attempt_recovery(base + 1);
      attempt_recovery(base + 2);
    }
  }
  close(sock);
  return NULL;
}

int main() {
  for (int i = 0; i < BUF_SIZE; i++) {
    atomic_init(&buffer[i].received, 0);
    atomic_init(&parity_buffer[i].received, 0);
  }

  char *to_str = getenv("T0");
  if (!to_str)
    to_str = getenv("TO");
  if (!to_str)
    to_str = getenv("t0");
  if (!to_str)
    to_str = getenv("to");

  char *delay_str = getenv("DELAY_MS");
  double delay_ms = delay_str ? atof(delay_str) : 60.0;
  double t0 = to_str ? atof(to_str) : get_now();

  printf("[RECEIVER DEBUG] Using T0 = %f (env: %s), DELAY_MS = %f\n", t0,
         to_str ? to_str : "NULL", delay_ms);
  fflush(stdout);

  pthread_t tid;
  pthread_create(&tid, NULL, receive_loop, NULL);

  int sock_out = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in addr_out;
  memset(&addr_out, 0, sizeof(addr_out));
  addr_out.sin_family = AF_INET;
  addr_out.sin_port = htons(OUT_PORT);
  addr_out.sin_addr.s_addr = inet_addr("127.0.0.1");

  uint8_t out_buf[RAW_FRAME_SIZE];
  uint32_t seq = 0;

  while (1) {
    double deadline = t0 + (delay_ms / 1000.0) + (seq * 0.020);

    // Priority 8 (Stage 1): Sleep until 2 milliseconds before deadline to
    // conserve CPU
    double sleep_target = deadline - 0.002;
    while (get_now() < sleep_target) {
      usleep(100);
    }

    uint32_t idx = seq % BUF_SIZE;

    // Priority 1 & 8 (Stage 2): Lock-free busy-spin until EXACTLY 20
    // MICROSECONDS before deadline! This completely eliminates Linux scheduler
    // wake-up latency right before sendto().
    while (get_now() < (deadline - 0.000020)) {
      if (atomic_load_explicit(&buffer[idx].received, memory_order_acquire) ==
          1) {
        break; // Frame arrived! Break instantly!
      }
      attempt_recovery(
          seq); // Continuous lock-free XOR recovery attempt while spinning
    }

    uint32_t net_seq = htonl(seq);
    memcpy(out_buf, &net_seq, 4);

    if (atomic_load_explicit(&buffer[idx].received, memory_order_acquire) ==
        1) {
      memcpy(out_buf + 4, buffer[idx].payload, PAYLOAD_SIZE);
      atomic_store_explicit(
          &buffer[idx].received, 0,
          memory_order_release); // Clear for next circular wrap
    } else {
      // Concealment: zero-fill if packet is definitively unrecoverable after
      // 20µs threshold
      memset(out_buf + 4, 0, PAYLOAD_SIZE);
    }

    sendto(sock_out, out_buf, RAW_FRAME_SIZE, 0, (struct sockaddr *)&addr_out,
           sizeof(addr_out));
    seq++;
  }

  close(sock_out);
  return 0;
}