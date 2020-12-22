#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <assert.h>
#include "../starter_files/PacketHeader.h"
#include "../starter_files/crc32.h"

#define BUFLEN 1472
#define PACKET_HEADER_LEN 16

using namespace std;

struct data_packet {
    char data[BUFLEN];
    unsigned int data_len;
    unsigned int seqNum;
    bool received;
};

void die(const char *s)
{
    perror(s);
    exit(1);
}

bool sumChecker (char * buf, int buf_len, unsigned int checksum) {
    return crc32(buf, buf_len) == checksum;
}

void send_message(int seqNum, int s, struct sockaddr_in & si_other, socklen_t & slen, ofstream & logging_file) {
    struct PacketHeader * header = new struct PacketHeader();
    header->type = 3;
    header->seqNum = seqNum;
    header->length = 0;
    header->checksum = 0;

    char packet[PACKET_HEADER_LEN];
    memcpy(packet, header, PACKET_HEADER_LEN);
    if (sendto(s, header, PACKET_HEADER_LEN, 0, (struct sockaddr*) &si_other, slen) == -1) {
        die("sendto()");
    }
    // printf("sent packet, seq number: %d\n", header->seqNum);
    logging_file << header->type << " " << header->seqNum << " " << header->length <<  " " << header->checksum << endl;
    delete header;
}

int main(int argc, char* argv[])
{
    // ./wReceiver <port-num> <window-size> <output-dir> <log>
    int port_num = atoi(argv[1]);
    int window_size = atoi(argv[2]);
    string output_dir = string(argv[3]);
    string log_file_name = string(argv[4]);

    struct sockaddr_in si_me, si_other;

    int s, recv_len;
    socklen_t slen = sizeof(si_other);
    char buf[BUFLEN];

    //create a UDP socket
    if ((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }

    // zero out the structure
    memset((char *) &si_me, 0, sizeof(si_me));

    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(port_num);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);

    //bind socket to port
    if( bind(s , (struct sockaddr*)&si_me, sizeof(si_me) ) == -1)
    {
        die("bind");
    }

    int connection_num = 0;
    ofstream output_file, logging_file;
    logging_file.open(log_file_name, ios_base::app);

    bool START_retrans = false;
    bool END_retrans = false;
    unsigned int start_seq = 0;
    int expected_seq;
    vector<data_packet *> window;

    struct data_packet * empty_packet = new struct data_packet();
    empty_packet->received = false;

    for (int i = 0; i < window_size; ++i) {
	    window.push_back(empty_packet);
    }

    //keep listening for data
    while (true)
    {
        //try to receive some data, this is a blocking call
        if ((recv_len = recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *) &si_other, &slen)) == -1)
        {
            die("recvfrom()");
        }

        struct PacketHeader * header = new struct PacketHeader();
        memcpy(header, buf, PACKET_HEADER_LEN);

        logging_file << header->type << " " << header->seqNum << " " << header->length << " " << header->checksum << endl;

        if (header->type == 0) {
            start_seq = header->seqNum;
            expected_seq = 0;
            // first START
            if (!START_retrans) {
                output_file.open(output_dir + "/FILE-" + to_string(connection_num) + ".out", ios_base::app);
                START_retrans = true;
                END_retrans = false;
            }
            send_message(start_seq, s, si_other, slen, logging_file);
        }

        // else if (header->type == 0 && middle_of_receiving) {
        //     continue;
        // }

        else if (header->type == 1) {
            // first END
            if (!END_retrans) {
                connection_num++;
                output_file.close();
                END_retrans = true;
                START_retrans = false;
            }
            send_message(start_seq, s, si_other, slen, logging_file);
        }
        
        else if (header->type == 2) {
            // printf("Received packet, seq number: %d\n", header->seqNum);

            if (header->seqNum >= expected_seq + window_size) {
                // printf("Out of window size\n");
                continue;
            }

            int data_len = header->length;
            char received_data[data_len];
            memcpy(received_data, buf + PACKET_HEADER_LEN, data_len);

            if (!sumChecker(received_data, data_len, header->checksum)){
                // printf("Wrong checksum\n");
                continue;
            }

            if (header->seqNum < expected_seq) {
                // printf("Smaller than execpted seqNum\n");
                send_message(expected_seq, s, si_other, slen, logging_file);
                continue;
            }
            
            struct data_packet * packet = new struct data_packet();
            memcpy(packet->data, received_data, data_len);
            packet->received = true;
            packet->seqNum = header->seqNum;
            packet->data_len = data_len;

            window[header->seqNum % window_size] = packet;
            
            int i = expected_seq;
            while (i < (expected_seq + window_size)) {
                if (window[i % window_size]->received) {
                    output_file.write(window[i % window_size]->data, window[i % window_size]->data_len);
                    delete window[i % window_size];
                    window[i % window_size] = empty_packet;
                    i++;
                }
                else break;
            } 

            expected_seq = i;
            send_message(expected_seq, s, si_other, slen, logging_file);
        }
    }
    logging_file.close();
    close(s);
    delete empty_packet;
    return 0;
}

