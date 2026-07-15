/* OPTIMIZED HYBRID RECEIVER (C) — Aggressive NACK + XOR FEC */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <unistd.h>

#define MAX_FRAMES 65536

int received[MAX_FRAMES] = {0};
unsigned char payloads[MAX_FRAMES][160];
double last_nack_time[MAX_FRAMES] = {0};

double get_now() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

int main(void) {
    char *t0_env = getenv("T0"), *delay_env = getenv("DELAY_MS");
    double t0 = t0_env ? atof(t0_env) : 0.0;
    double delay_s = delay_env ? atof(delay_env) / 1000.0 : 0.0;

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr)) < 0) return 1;

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    int nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay_fb = {0};
    relay_fb.sin_family = AF_INET;
    relay_fb.sin_port = htons(47003);
    relay_fb.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];
    int highest_seq = -1;
    double last_bg_check = get_now();

    for (;;) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(in_fd, &read_fds);

        struct timeval tv = {0, 10000}; 
        int ret = select(in_fd + 1, &read_fds, NULL, NULL, &tv);
        double now = get_now();

        if (ret > 0 && FD_ISSET(in_fd, &read_fds)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof(buf), 0, NULL, NULL);
            if (n >= 164) {
                uint32_t net_seq;
                memcpy(&net_seq, buf, 4);
                uint32_t raw_seq = ntohl(net_seq);
                
                int is_fec = (raw_seq & 0x80000000) != 0;
                uint32_t host_seq = raw_seq & 0x7FFFFFFF;

                if (!is_fec) {
                    if (host_seq < MAX_FRAMES && !received[host_seq]) {
                        received[host_seq] = 1;
                        memcpy(payloads[host_seq], buf + 4, 160);
                        sendto(out_fd, buf, n, 0, (struct sockaddr *)&player, sizeof(player));
                        if ((int)host_seq > highest_seq) highest_seq = (int)host_seq;
                    }
                } else { 
                    uint32_t seq1 = host_seq - 1;
                    uint32_t seq2 = host_seq;
                    if (seq1 < MAX_FRAMES && seq2 < MAX_FRAMES) {
                        uint32_t recovered_seq = 0xFFFFFFFF;
                        if (received[seq1] && !received[seq2]) recovered_seq = seq2;
                        else if (!received[seq1] && received[seq2]) recovered_seq = seq1;

                        if (recovered_seq != 0xFFFFFFFF) {
                            unsigned char rec_buf[164];
                            uint32_t net_rec = htonl(recovered_seq);
                            memcpy(rec_buf, &net_rec, 4);
                            uint32_t known_seq = (recovered_seq == seq1) ? seq2 : seq1;
                            
                            for (int i = 0; i < 160; i++) {
                                rec_buf[4+i] = payloads[known_seq][i] ^ buf[4+i];
                            }
                            
                            received[recovered_seq] = 1;
                            memcpy(payloads[recovered_seq], rec_buf + 4, 160);
                            sendto(out_fd, rec_buf, 164, 0, (struct sockaddr *)&player, sizeof(player));
                        }
                    }
                }
            }
        }

        now = get_now();
        if (now - last_bg_check >= 0.010) {
            last_bg_check = now;
            int start_nack = t0 > 0.0 ? (int)((now - t0 - delay_s) / 0.020) : 0;
            if (start_nack < 0) start_nack = 0;

            uint32_t nacks[200];
            int nack_count = 0;

            for (int i = start_nack; i < highest_seq && nack_count < 200; i++) {
                // Aggressive 25ms throttle
                if (!received[i] && (now - last_nack_time[i] >= 0.025)) {
                    last_nack_time[i] = now;
                    nacks[nack_count++] = htonl(i);
                }
            }
            if (nack_count > 0) {
                sendto(nack_fd, nacks, nack_count * 4, 0, (struct sockaddr *)&relay_fb, sizeof(relay_fb));
            }
        }
    }
    return 0;
}