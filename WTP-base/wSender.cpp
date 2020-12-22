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

bool receive_from(int sockfd, sockaddr_in *src_addr, socklen_t *slen,
                 struct PacketHeader* header, chrono::time_point<chrono::steady_clock> window_start_time,
                 unsigned int window_start, ofstream & logging_file) {
    char buffer[PACKET_HEADER_LEN];
    int recv_len = 0;
    bool time_out = true;
    
    auto current_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = current_time - window_start_time;
    
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
            if (header->type == 3 && header->seqNum > window_start) {
                time_out = false;
                break;
            }
        }
        current_time = std::chrono::steady_clock::now();
        diff = current_time - window_start_time;
    }
    return time_out;
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
        bool time_out = receive_from(sockfd, &src_addr, &slen, &header_recv, window_start_time, seqRand-1, logging_file);
        if (!time_out) {
            // printf("Received start packet, seq number: %u\n", header_recv.seqNum);
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

    int file_start_pos = window_end;

    while (true) {
        // send
        auto window_start_time = chrono::steady_clock::now();
        while (window_end < (window_start + window_size) && window_end < total_packets) {
            char packet[PACKET_LEN];
            int packet_data_size = (window_end == total_packets - 1) ? (to_send_length - PACKET_DATA_LEN * (window_end - file_start_pos)) : PACKET_DATA_LEN;
            memcpy(packet + PACKET_HEADER_LEN, to_send + ((window_end - file_start_pos) * PACKET_DATA_LEN), packet_data_size);
            
            // add header to packet
            struct PacketHeader header;
            header.type = 2;
            header.length = packet_data_size;
            header.seqNum = window_end;
            header.checksum = crc32(packet + PACKET_HEADER_LEN, packet_data_size);
            memcpy(packet, &header, PACKET_HEADER_LEN);

            int send_len = sendto(sockfd, packet, packet_data_size + PACKET_HEADER_LEN, 0, (struct sockaddr *) &si_other, sizeof(si_other));
            if (send_len == -1) {
                die("sendto()");
            }
            logging_file << header.type << " " << header.seqNum << " "
                         << header.length << " " << header.checksum << endl;
            window_end++;
        }

        // listen
        while (true) {
            struct sockaddr_in src_addr;
            socklen_t slen = sizeof(src_addr);
            struct PacketHeader header_recv;
            bool time_out = receive_from(sockfd, &src_addr, &slen, &header_recv, window_start_time, window_start, logging_file);
            if (time_out) {
                // timeout
                // printf("seq number %d timed out\n", window_start);
                window_end = window_start;
                break;
            }
            // printf("Received data packet, seq number: %u, window_start: %d, window_end: %d\n", header_recv.seqNum, window_start, window_end);
            if (header_recv.seqNum <= window_start || header_recv.seqNum > window_end) {
                continue;
            }

            window_start = header_recv.seqNum;
            window_end = max(window_start, window_end);
            if (window_end < total_packets || window_start >= total_packets) {
                break;
            }
        }
        if (window_start >= total_packets) {
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
            time_out = receive_from(sockfd, &src_addr, &slen, &header_recv, window_start_time, seqStart-1, logging_file);
            // printf("Received end packet, seq number: %u\n", header_recv.seqNum);
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

    int filesize_in_byte = 0;
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
        filesize_in_byte += (message_size == -1) ? MAX_MESSAGE_LEN : message_size;
        send_file_to_receiver(s, message, message_size, si_other,
                              window_size, window_start, window_end, logging_file);
        if (message_size != -1) {
            break;
        }
    }
    
    printf("file size: %d\n", filesize_in_byte);
    send_end_to_receiver(s, seqStart, si_other, logging_file);
    logging_file.close();
    close(s);
    
    return 0;
}


