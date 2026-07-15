/* OPTIMIZED HYBRID SENDER (C) — Aggressive Double-Tap ARQ + XOR FEC */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#define PAYLOAD_SIZE 160
#define MAX_FRAMES 65536

struct frame {
    uint32_t net_seq;
    unsigned char payload[PAYLOAD_SIZE];
    int valid;
};

struct frame history[MAX_FRAMES];

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) return 1;

    int nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in nack_addr = {0};
    nack_addr.sin_family = AF_INET;
    nack_addr.sin_port = htons(47004);
    nack_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(nack_fd, (struct sockaddr *)&nack_addr, sizeof(nack_addr)) < 0) return 1;

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    memset(history, 0, sizeof(history));
    int max_fd = (in_fd > nack_fd) ? in_fd : nack_fd;
    unsigned char buf[2048];

    for (;;) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(in_fd, &read_fds);
        FD_SET(nack_fd, &read_fds);

        select(max_fd + 1, &read_fds, NULL, NULL, NULL);

        if (FD_ISSET(in_fd, &read_fds)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n >= (ssize_t)(4 + PAYLOAD_SIZE)) {
                uint32_t net_seq;
                memcpy(&net_seq, buf, 4);
                uint32_t host_seq = ntohl(net_seq);

                if (host_seq < MAX_FRAMES) {
                    history[host_seq].net_seq = net_seq;
                    memcpy(history[host_seq].payload, buf + 4, PAYLOAD_SIZE);
                    history[host_seq].valid = 1;
                }
                sendto(out_fd, buf, n, 0, (struct sockaddr *)&relay, sizeof(relay));

                if (host_seq % 2 == 1 && history[host_seq - 1].valid) {
                    unsigned char fec_buf[164];
                    uint32_t fec_seq = host_seq | 0x80000000; 
                    uint32_t net_fec_seq = htonl(fec_seq);
                    memcpy(fec_buf, &net_fec_seq, 4);
                    
                    for (int i = 0; i < PAYLOAD_SIZE; i++) {
                        fec_buf[4 + i] = history[host_seq - 1].payload[i] ^ history[host_seq].payload[i];
                    }
                    sendto(out_fd, fec_buf, 164, 0, (struct sockaddr *)&relay, sizeof(relay));
                }
            }
        }

        // --- ARQ NACK HANDLING ---
        if (FD_ISSET(nack_fd, &read_fds)) {
            ssize_t n = recvfrom(nack_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n > 0 && n % 4 == 0) {
                for (int i = 0; i < n; i += 4) {
                    uint32_t req_net_seq;
                    memcpy(&req_net_seq, buf + i, 4);
                    uint32_t req_host_seq = ntohl(req_net_seq);

                    if (req_host_seq < MAX_FRAMES && history[req_host_seq].valid) {
                        unsigned char resend_buf[164];
                        memcpy(resend_buf, &history[req_host_seq].net_seq, 4);
                        memcpy(resend_buf + 4, history[req_host_seq].payload, PAYLOAD_SIZE);
                        
                        // DOUBLE-TAP: Send it twice to guarantee it survives a burst drop
                        sendto(out_fd, resend_buf, 164, 0, (struct sockaddr *)&relay, sizeof(relay));
                        sendto(out_fd, resend_buf, 164, 0, (struct sockaddr *)&relay, sizeof(relay));
                    }
                }
            }
        }
    }
    return 0;
}