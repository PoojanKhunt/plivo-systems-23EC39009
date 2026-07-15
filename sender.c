#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IN_PORT 47010
#define OUT_PORT 47001
#define PAYLOAD_SIZE 160
#define RAW_FRAME_SIZE 164

// Packed wire structures for explicit Data and Parity identification
struct __attribute__((packed)) DataPacket {
  uint8_t type; // 0 = Primary Data
  uint32_t seq;
  uint8_t payload[PAYLOAD_SIZE];
};

struct __attribute__((packed)) ParityPacket {
  uint8_t type; // 1 = Overlapping XOR Parity
  uint32_t base_seq;
  uint8_t parity[PAYLOAD_SIZE];
};

uint8_t history[1024][PAYLOAD_SIZE];

int main() {
  int sock_in = socket(AF_INET, SOCK_DGRAM, 0);
  int sock_out = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock_in < 0 || sock_out < 0) {
    perror("Socket creation failed");
    return 1;
  }

  int reuse = 1;
  setsockopt(sock_in, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr_in, addr_out;
  memset(&addr_in, 0, sizeof(addr_in));
  addr_in.sin_family = AF_INET;
  addr_in.sin_port = htons(IN_PORT);
  addr_in.sin_addr.s_addr = inet_addr("127.0.0.1");

  if (bind(sock_in, (struct sockaddr *)&addr_in, sizeof(addr_in)) < 0) {
    perror("Bind failed on port 47010");
    return 1;
  }

  memset(&addr_out, 0, sizeof(addr_out));
  addr_out.sin_family = AF_INET;
  addr_out.sin_port = htons(OUT_PORT);
  addr_out.sin_addr.s_addr = inet_addr("127.0.0.1");

  uint8_t in_buf[RAW_FRAME_SIZE];
  memset(history, 0, sizeof(history));

  while (1) {
    ssize_t n = recv(sock_in, in_buf, sizeof(in_buf), 0);
    if (n != RAW_FRAME_SIZE)
      continue;

    uint32_t net_seq;
    memcpy(&net_seq, in_buf, 4);
    uint32_t seq = ntohl(net_seq);

    // Maintain sliding history window for XOR computation
    memcpy(history[seq % 1024], in_buf + 4, PAYLOAD_SIZE);

    // 1. Send Primary Data Packet (165 bytes wire format)
    struct DataPacket data_pkt;
    data_pkt.type = 0;
    data_pkt.seq = htonl(seq);
    memcpy(data_pkt.payload, in_buf + 4, PAYLOAD_SIZE);
    sendto(sock_out, &data_pkt, sizeof(data_pkt), 0,
           (struct sockaddr *)&addr_out, sizeof(addr_out));

    // 2. Sliding Window Parity: Send overlapping XOR block on 8 out of 10
    // frames (~1.85x overhead)
    if (seq >= 2 && seq % 5 != 0) {
      uint32_t base_seq = seq - 2;
      struct ParityPacket parity_pkt;
      parity_pkt.type = 1;
      parity_pkt.base_seq = htonl(base_seq);

      // Generate bitwise parity across sliding triad {base_seq, base_seq+1,
      // base_seq+2}
      for (int j = 0; j < PAYLOAD_SIZE; j++) {
        parity_pkt.parity[j] = history[base_seq % 1024][j] ^
                               history[(base_seq + 1) % 1024][j] ^
                               history[(base_seq + 2) % 1024][j];
      }
      sendto(sock_out, &parity_pkt, sizeof(parity_pkt), 0,
             (struct sockaddr *)&addr_out, sizeof(addr_out));
    }
  }

  close(sock_in);
  close(sock_out);
  return 0;
}