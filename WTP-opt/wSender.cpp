#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <math.h>
#include <errno.h>
#include <assert.h>
#include <chrono>
#include "../starter_files/PacketHeader.h"
#include "../starter_files/crc32.h"

#define BUFLEN 512
#define MAX_MESSAGE_LEN 145600
#define PACKET_LEN 1472
#define PACKET_HEADER_LEN 16
#define PACKET_DATA_LEN 1456
#define TIME_OUT 0.5

using namespace std;

struct packet_status {
    unsigned int seqNum;
    bool is_ack;
    chrono::time_point<chrono::steady_clock> sent_time;
    int data_size;
    int data_offset;
};


void die(const char *s) {
    perror(s);
    exit(1);
}

int read_from_file(ifstream & myfile, char * message) {
    myfile.read(message, MAX_MESSAGE_LEN);
    if (myfile.eof()) {
        int num = myfile.gcount();
        myfile.close();
        return num;
    }
    return -1;
}

int find_earliest_send_time (vector<packet_status *> & window, int window_start) {
    // find the lastest send time of unacknowledged packet in the window
    assert(!window[window_start % window.size()]->is_ack);
    int max_index = window_start % window.size();
    chrono::time_point<chrono::steady_clock> max_time = window[window_start % window.size()]->sent_time;

    for (int i = (window_start + 1); i < (window_start + window.size()); ++i) {
        if (!window[i % window.size()]->is_ack && (window[i % window.size()]->sent_time - max_time).count() < 0) {
            max_index = i % window.size();
            max_time = window[i % window.size()]->sent_time;
        }
    }
    return max_index;
}

bool receive_start_end_from(int sockfd, sockaddr_in *src_addr, socklen_t *slen,
                 struct PacketHeader* header, chrono::time_point<chrono::steady_clock> start_time,
                 unsigned int expected_seqNum, ofstream & logging_file) {
    char buffer[PACKET_HEADER_LEN];
    int recv_len = 0;
    bool time_out = true;
    
    auto current_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = current_time - start_time;
    
    while (diff.count() < TIME_OUT) {
        recv_len = recvfrom(sockfd, buffer, PACKET_HEADER_LEN, 0, (struct sockaddr *) src_addr, slen);
        if (recv_len == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                die("recvfrom()");
            }
        }
        else {
            memcpy(header, buffer, PACKET_HEADER_LEN);
            logging_file << header->type << " " << header->seqNum << " "
                         << header->length << " " << header->checksum << endl;

            // printf("Received start_end packet, seq number: %u\n", header->seqNum);
            if (header->type == 3 && header->seqNum == expected_seqNum) {
                time_out = false;
                break;
            }
        }
        current_time = std::chrono::steady_clock::now();
        diff = current_time - start_time;
    }
    return time_out;
}

bool receive_data_from(int sockfd, sockaddr_in *src_addr, socklen_t *slen,
                 struct PacketHeader* header, chrono::time_point<chrono::steady_clock> start_time,
                 unsigned int expected_seqNum, int window_start, ofstream & logging_file, vector<packet_status *> & window) {
    char buffer[PACKET_HEADER_LEN];
    int recv_len = 0;
    bool time_out = true;
    
    auto current_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = current_time - start_time;
    
    while (diff.count() < TIME_OUT) {
        recv_len = recvfrom(sockfd, buffer, PACKET_HEADER_LEN, 0, (struct sockaddr *) src_addr, slen);
        if (recv_len == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                die("recvfrom()");
            }
        }
        else {
            memcpy(header, buffer, PACKET_HEADER_LEN);
            logging_file << header->type << " " << header->seqNum << " "
                         << header->length << " " << header->checksum << endl;

            if (header->seqNum < window_start || header->seqNum >= (window_start + window.size())) {
                continue;
            }
            window[header->seqNum % window.size()]->is_ack = true;
            // printf("Received data packet, seq number: %u\n", header->seqNum);
            if (header->type == 3 && header->seqNum == expected_seqNum) {
                time_out = false;
                break;
            }
        }
        current_time = std::chrono::steady_clock::now();
        diff = current_time - start_time;
    }
    return time_out;
}

void send_data_packet(int sockfd, struct sockaddr_in si_other, int packet_data_size, const char * to_send, int offset, 
                int seqNum, vector<packet_status *> & window, ofstream & logging_file) {
    
    char packet[PACKET_LEN];
    memcpy(packet + PACKET_HEADER_LEN, to_send + offset, packet_data_size);
    
    // add header to packet
    struct PacketHeader header;
    header.type = 2;
    header.length = packet_data_size;
    header.seqNum = seqNum;
    header.checksum = crc32(packet + PACKET_HEADER_LEN, packet_data_size);
    memcpy(packet, &header, PACKET_HEADER_LEN);

    struct packet_status * status = new struct packet_status();
    status->is_ack = false;
    status->seqNum = seqNum;
    status->sent_time = chrono::steady_clock::now();
    status->data_size = packet_data_size;
    status->data_offset = offset;

    window[seqNum % window.size()] = status;

    int send_len = sendto(sockfd, packet, packet_data_size + PACKET_HEADER_LEN, 0, (struct sockaddr *) &si_other, sizeof(si_other));
    if (send_len == -1) {
        die("sendto()");
    }
    // printf("Send data packet: %d\n", seqNum);
    logging_file << header.type << " " << header.seqNum << " "
                    << header.length << " " << header.checksum << endl;
}

int send_start_to_receiver(int sockfd, struct sockaddr_in si_other, ofstream & logging_file) {
    struct PacketHeader header;
    unsigned int seqRand = (unsigned int) rand();
    // printf("start seq number is %u\n", seqRand);

    header.type = 0;
    header.length = 0;
    header.seqNum = seqRand;
    header.checksum = 0;
    char packet[PACKET_HEADER_LEN];
    memcpy(packet, &header, PACKET_HEADER_LEN);

    struct sockaddr_in src_addr;
    socklen_t slen = sizeof(src_addr);
    while (true) {
        auto window_start_time = chrono::steady_clock::now();
        int send_len = sendto(sockfd, (void *) packet, PACKET_HEADER_LEN, 0, (struct sockaddr *) &si_other, sizeof(si_other));
        if (send_len == -1) {
            die("sendto()");
        }
        logging_file << header.type << " " << header.seqNum << " "
                     << header.length << " " << header.checksum << endl;
        // recv ACK
        struct PacketHeader header_recv;
        bool time_out = receive_start_end_from(sockfd, &src_addr, &slen, &header_recv, window_start_time, seqRand, logging_file);
        if (!time_out) {
            break;
        }
    }
    return seqRand;
}

void send_file_to_receiver(int sockfd, const char* to_send, int to_send_length,
                          struct sockaddr_in si_other, int window_size, int & window_start, int & window_end,
                          ofstream & logging_file) {
    if (to_send_length == -1) {
        to_send_length = MAX_MESSAGE_LEN;
    }
    int total_packets = (int) ceil(1.0 * to_send_length / PACKET_DATA_LEN) + window_end;

    // printf("total_packets = %d\n", total_packets);
    int file_start_pos = window_end;

    vector<struct packet_status *> window;

    struct packet_status * empty_status = new struct packet_status();
    empty_status->is_ack = true;

    for (int i = 0; i < window_size; ++i) {
        window.push_back(empty_status);
    }

    // printf("--------------------------------\n");

    while (true) {
        // send
        while (window_end < (window_start + window_size) && window_end < total_packets) {
            int packet_data_size = (window_end == total_packets - 1) ? (to_send_length - PACKET_DATA_LEN * (window_end - file_start_pos)) : PACKET_DATA_LEN;
            send_data_packet(sockfd, si_other, packet_data_size, to_send, ((window_end - file_start_pos) * PACKET_DATA_LEN), window_end, window, logging_file);
            window_end++;
        }

        // listen
        while (true) {
            int earliest_send_index = find_earliest_send_time(window, window_start);
            auto earliest_send_time = window[earliest_send_index]->sent_time;
            // printf("The earliest send seq number is %d\n", window[earliest_send_index]->seqNum);

            struct sockaddr_in src_addr;
            socklen_t slen = sizeof(src_addr);
            struct PacketHeader header_recv;

            bool time_out = receive_data_from(sockfd, &src_addr, &slen, &header_recv, earliest_send_time, window[earliest_send_index]->seqNum, window_start, logging_file, window);
            if (time_out) {
                // timeout
                // printf("seq number %d timed out\n", window[earliest_send_index]->seqNum);
                send_data_packet(sockfd, si_other, window[earliest_send_index]->data_size, to_send, window[earliest_send_index]->data_offset, window[earliest_send_index]->seqNum, window, logging_file);
            }

            int i = window_start;
            while (i < (window_start + window_size) && i < window_end) {
                assert(window[i % window_size]->seqNum == i);
                // printf("seq num %d ack?: %s\n", i, window[i % window_size]->is_ack ? "true" : "false");
                if (window[i % window_size]->is_ack) {
                    delete window[i % window_size];
                    window[i % window_size] = empty_status;
                    i++;
                }
                else break;
            }

            if (window_start == i) continue;

            window_start = i;
            // printf("window_start: %d, window_end: %d\n", window_start, window_end);
            // printf("--------------------------------\n");

            if (window_end < total_packets || window_start == total_packets) {
                break;
            }
        }
        if (window_start == total_packets) {
            return;
        }
    }
}

void send_end_to_receiver(int sockfd, int seqStart, struct sockaddr_in si_other, ofstream & logging_file) {
    struct PacketHeader header;
    header.type = 1;
    header.length = 0;
    header.seqNum = seqStart;
    header.checksum = 0;
    char packet[PACKET_HEADER_LEN];
    memcpy(packet, &header, PACKET_HEADER_LEN);
    struct sockaddr_in src_addr;
    socklen_t slen = sizeof(src_addr);

    while (true) {
        auto window_start_time = chrono::steady_clock::now();
        int send_len = sendto(sockfd, packet, PACKET_HEADER_LEN, 0, (struct sockaddr *) &si_other, sizeof(si_other));
        if (send_len == -1) {
            die("sendto()");
        }
        logging_file << header.type << " " << header.seqNum << " "
                     << header.length << " " << header.checksum << endl;

        bool time_out = false;
        bool end_ack = false;
        while (true) {
            // recv ACK
            struct PacketHeader header_recv;
            time_out = receive_start_end_from(sockfd, &src_addr, &slen, &header_recv, window_start_time, seqStart, logging_file);
            if (time_out) break;
            if (header_recv.type == 3 && header_recv.seqNum == seqStart) {
                end_ack = true;
                break;
            }
        }
        if (end_ack) break;
    }
}

int main(int argc, char* argv[]) {
    // ./wSender <receiver-IP> <receiver-port> <window-size> <input-file> <log>
    int receiver_port = atoi(argv[2]);
    int window_size = atoi(argv[3]);
    string input_file = string(argv[4]);

    ofstream logging_file;
    logging_file.open(string(argv[5]), ios_base::app);

    struct sockaddr_in si_other;
    int s;
    if ( (s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
        die("socket");
    }
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        die("Error setting timeout info");
    }

    memset((char *) &si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(receiver_port);
    si_other.sin_addr.s_addr = inet_addr(argv[1]);
    
    int window_start = 0;
    int window_end = 0;

    ifstream myfile(input_file); 
    int seqStart = send_start_to_receiver(s, si_other, logging_file);

    while(true) {
        // read file content
        char message[MAX_MESSAGE_LEN]; 
        int message_size = read_from_file(myfile, message);
        send_file_to_receiver(s, message, message_size, si_other,
                              window_size, window_start, window_end, logging_file);
        if (message_size != -1) {
            break;
        }
    }
    
    send_end_to_receiver(s, seqStart, si_other, logging_file);
    logging_file.close();
    close(s);

    
    
    return 0;
}


