#include "Common/config.h"
#include "Http/HttpSession.h"
#include "Network/TcpServer.h"
#include "Rtmp/RtmpSession.h"
#include "Rtp/RtpProcess.h"
#include "Rtsp/RtspSession.h"
#include "Util/logger.h"
#include "Util/util.h"
#include <iostream>
#include <map>
#include <pcap.h>

using namespace std;
using namespace toolkit;
using namespace mediakit;

/* Ethernet frame header */
struct sniff_ethernet {
#define ETHER_ADDR_LEN 6
    u_char ether_dhost[ETHER_ADDR_LEN]; /* The address of the destination host */
    u_char ether_shost[ETHER_ADDR_LEN]; /* The address of the source host */
    u_short ether_unused;
    u_short ether_type; /* IP：0x0800;IPV6:0x86DD; ARP:0x0806;RARP:0x8035 */
};

#define ETHERTYPE_IPV4 (0x0800)
#define ETHERTYPE_IPV6 (0x86DD)
#define ETHERTYPE_ARP (0x0806)
#define ETHERTYPE_RARP (0x8035)

/* Header of IP packet */
struct sniff_ip {
#if BYTE_ORDER == LITTLE_ENDIAN
    u_int ip_hl : 4, /* Head length */
        ip_v : 4;    /* Version number */
#if BYTE_ORDER == BIG_ENDIAN
    u_int ip_v : 4, /* Version number */
        ip_hl : 4;  /* Head length */
#endif
#endif              /* not _IP_VHL */
    u_char ip_tos;  /* Type of service */
    u_short ip_len; /* Total length */
    u_short ip_id;  /* Package logo number */
    u_char ip_flag;
    u_char ip_off;                 /* Fragment offset */
#define IP_RF 0x8000               /* Retained fragment logo */
#define IP_DF 0x4000               /* dont fragment flag */
#define IP_MF 0x2000               /* Multi-fragment logo*/
#define IP_OFFMASK 0x1fff          /* Segment */
    u_char ip_ttl;                 /* The survival time of the packet */
    u_char ip_p;                   /* The protocol used:1 ICMP;2 IGMP;4 IP;6 TCP;17 UDP;89 OSPF */
    u_short ip_sum;                /* Checksum */
    struct in_addr ip_src, ip_dst; /* Source address, destination address*/
};
#define IPTYPE_ICMP (1)
#define IPTYPE_IGMP (2)
#define IPTYPE_IP (4)
#define IPTYPE_TCP (6)
#define IPTYPE_UDP (17)
#define IPTYPE_OSPF (89)

typedef u_int tcp_seq;
/* TCP The header of the packet */
struct sniff_tcp {
    u_short th_sport; /* Source port */
    u_short th_dport; /* Destination port */
    tcp_seq th_seq;   /* Package number */
    tcp_seq th_ack;   /* Confirm serial number */
#if BYTE_ORDER == LITTLE_ENDIAN
    u_int th_x2 : 4, /* Not used yet */
        th_off : 4;  /* Data offset */
#endif
#if BYTE_ORDER == BIG_ENDIAN
    u_int th_off : 4, /* Data offset*/
        th_x2 : 4;    /*Not used yet */
#endif
    u_char th_flags;
#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PUSH 0x08
#define TH_ACK 0x10
#define TH_URG 0x20
#define TH_ECE 0x40
#define TH_CWR 0x80
#define TH_FLAGS (TH_FINTH_SYNTH_RSTTH_ACKTH_URGTH_ECETH_CWR)
    u_short th_win; /* TCP sliding window */
    u_short th_sum; /* Head checksum */
    u_short th_urp; /* Emergency service */
};

/* UDP header */
struct sniff_udp {
    uint16_t sport; /* source port */
    uint16_t dport; /* destination port */
    uint16_t udp_length;
    uint16_t udp_sum; /* checksum */
};

struct rtp_stream {
    uint64_t stamp = 0;
    uint64_t stamp_last = 0;
    std::shared_ptr<RtpProcess> rtp_process;
    Socket::Ptr sock;
    struct sockaddr_storage addr;
};
static semaphore sem;
unordered_map<uint32_t, rtp_stream> rtp_streams_map;

#if defined(ENABLE_RTPPROXY)
void processRtp(uint32_t stream_id, const char *rtp, int &size, bool is_udp, const EventPoller::Ptr &poller) {
    rtp_stream &stream = rtp_streams_map[stream_id];
    if (!stream.rtp_process) {
        auto process = RtpProcess::createProcess(MediaTuple{DEFAULT_VHOST, kRtpAppName, to_string(stream_id), ""});
        stream.rtp_process = process;
        struct sockaddr_storage addr;
        memset(&addr, 0, sizeof(addr));
        addr.ss_family = AF_INET;
        auto sock = Socket::createSocket(poller);
        stream.sock = sock;
        stream.addr = addr;
    }

    try {
        stream.rtp_process->inputRtp(is_udp, stream.sock, rtp, size, (struct sockaddr *)&stream.addr, &stream.stamp);
    } catch (std::exception &ex) {
        WarnL << "Input rtp failed: " << ex.what();
        return ;
    }

    auto diff = static_cast<int64_t>(stream.stamp - stream.stamp_last);
    if (diff > 0 && diff < 500) {
        usleep(diff * 1000);
    } else {
        usleep(1 * 1000);
    }
    stream.stamp_last = stream.stamp;

    rtp = nullptr;
    size = 0;
}
#endif // #if defined(ENABLE_RTPPROXY)

static bool loadFile(const char *path, const EventPoller::Ptr &poller) {
    char errbuf[PCAP_ERRBUF_SIZE] = {'\0'};
    std::shared_ptr<pcap_t> handle(pcap_open_offline(path, errbuf), [](pcap_t *handle) {
        sem.post();
        if (handle) {
            pcap_close(handle);
        }
    });
    if (!handle) {
        WarnL << "open file failed:" << path << "error: " << errbuf;
        return false;
    }
    auto total_size = std::make_shared<size_t>(0);
    struct pcap_pkthdr header = {0};
    while (true) {
        const u_char *pkt_buff = pcap_next(handle.get(), &header);
        if (!pkt_buff) {
            PrintE("pcapng read over.");
            break;
        }

        struct sniff_ethernet *ethernet = (struct sniff_ethernet *)pkt_buff;
        int eth_len = sizeof(struct sniff_ethernet);  // The length of the Ethernet header
        int ip_len = sizeof(struct sniff_ip);         // Length of IP head
        int tcp_len = sizeof(struct sniff_tcp);       // Length of tcp header
        int udp_headr_len = sizeof(struct sniff_udp); // Length of udp header

        /*Parse the network layer IP header*/
        if (ntohs(ethernet->ether_type) == ETHERTYPE_IPV4) { // IPV4
            struct sniff_ip *ip = (struct sniff_ip *)(pkt_buff + eth_len);
            ip_len = (ip->ip_hl & 0x0f) * 4;                            // Length of IP head
            unsigned char *saddr = (unsigned char *)&ip->ip_src.s_addr; // Convert network byte order to host byte order
            unsigned char *daddr = (unsigned char *)&ip->ip_dst.s_addr;
            /*Analyze the transport layer  TCP、UDP、ICMP*/
            if (ip->ip_p == IPTYPE_TCP) { // TCP
                PrintI("ip->proto:TCP "); // Which protocol is used in the transport layer
                struct sniff_tcp *tcp = (struct sniff_tcp *)(pkt_buff + eth_len + ip_len);
                PrintI("tcp_sport = %u ", tcp->th_sport);
                PrintI("tcp_dport = %u ", tcp->th_dport);
                for (int i = 0; *(pkt_buff + eth_len + ip_len + tcp_len + i) != '\0'; i++) {
                    PrintI("%02x ", *(pkt_buff + eth_len + ip_len + tcp_len + i));
                }
            } else if (ip->ip_p == IPTYPE_UDP) { // UDP
                // PrintI("ip->proto:UDP ");        // Which protocol is used in the transport layer
                struct sniff_udp *udp = (struct sniff_udp *)(pkt_buff + eth_len + ip_len);
                auto udp_pack_len = ntohs(udp->udp_length);

                uint32_t src_ip = ntohl(ip->ip_src.s_addr);
                uint32_t dst_ip = ntohl(ip->ip_dst.s_addr);
                uint16_t src_port = ntohs(udp->sport);
                uint16_t dst_port = ntohs(udp->dport);
                uint32_t stream_id = (src_ip << 16) + src_port + (dst_ip << 4) + dst_port;

                const char *rtp = reinterpret_cast<const char *>(pkt_buff + eth_len + ip_len + udp_headr_len);
                auto rtp_len = udp_pack_len - udp_headr_len;
#if defined(ENABLE_RTPPROXY)
                processRtp(stream_id, rtp, rtp_len, true, poller);
#endif                                            // #if defined(ENABLE_RTPPROXY)
            } else if (ip->ip_p == IPTYPE_ICMP) { // ICMP
                PrintI("ip->proto:CCMP ");        // Which protocol is used in the transport layer
            } else {
                PrintI("Unidentified Transport Layer Protocol");
            }

        } else if (ntohs(ethernet->ether_type) == ETHERTYPE_IPV6) { // IPV6
            PrintI("It's IPv6! ");
        } else {
            PrintI("Neither IPV4 nor IPV6 ");
        }
    }

    return true;
}

int main(int argc, char *argv[]) {
    // Setting up logs
    Logger::Instance().add(std::make_shared<ConsoleChannel>("ConsoleChannel"));

    // Start an asynchronous log thread
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());
    loadIniConfig((exeDir() + "config.ini").data());

    TcpServer::Ptr rtspSrv(new TcpServer());
    TcpServer::Ptr rtmpSrv(new TcpServer());
    TcpServer::Ptr httpSrv(new TcpServer());
    rtspSrv->start<RtspSession>(554);  // Default 554
    rtmpSrv->start<RtmpSession>(1935); // Default 1935
    httpSrv->start<HttpSession>(81);   // Default 80

    if (argc == 2) {
        auto poller = EventPollerPool::Instance().getPoller();
        poller->async_first([poller, argv]() {
            loadFile(argv[1], poller);
            sem.post();
        });
        sem.wait();
        sleep(1);
    } else {
        ErrorL << "parameter error.";
    }

    return 0;
}