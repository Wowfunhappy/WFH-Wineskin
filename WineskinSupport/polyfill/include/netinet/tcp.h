/* netinet/tcp.h with TCP_CONNECTION_INFO and its structure (10.11 SDK). src/getsockopt.c answers it. */
#include_next <netinet/tcp.h>

#if !defined(WINESKIN_NETINET_TCP_H) && !defined(TCP_CONNECTION_INFO)
#define WINESKIN_NETINET_TCP_H

#define TCP_CONNECTION_INFO 0x106

#define TCPCI_OPT_TIMESTAMPS    0x00000001
#define TCPCI_OPT_SACK          0x00000002
#define TCPCI_OPT_WSCALE        0x00000004
#define TCPCI_OPT_ECN           0x00000008

#define TCPCI_FLAG_LOSSRECOVERY 0x00000001
#define TCPCI_FLAG_REORDERING_DETECTED 0x00000002

struct tcp_connection_info {
    u_int8_t   tcpi_state;
    u_int8_t   tcpi_snd_wscale;
    u_int8_t   tcpi_rcv_wscale;
    u_int8_t   __pad1;
    u_int32_t  tcpi_options;
    u_int32_t  tcpi_flags;
    u_int32_t  tcpi_rto;
    u_int32_t  tcpi_maxseg;
    u_int32_t  tcpi_snd_ssthresh;
    u_int32_t  tcpi_snd_cwnd;
    u_int32_t  tcpi_snd_wnd;
    u_int32_t  tcpi_snd_sbbytes;
    u_int32_t  tcpi_rcv_wnd;
    u_int32_t  tcpi_rttcur;
    u_int32_t  tcpi_srtt;
    u_int32_t  tcpi_rttvar;
    u_int32_t
        tcpi_tfo_cookie_req:1,
        tcpi_tfo_cookie_rcv:1,
        tcpi_tfo_syn_loss:1,
        tcpi_tfo_syn_data_sent:1,
        tcpi_tfo_syn_data_acked:1,
        tcpi_tfo_syn_data_rcv:1,
        tcpi_tfo_cookie_req_rcv:1,
        tcpi_tfo_cookie_sent:1,
        tcpi_tfo_cookie_invalid:1,
        tcpi_tfo_cookie_wrong:1,
        tcpi_tfo_no_cookie_rcv:1,
        tcpi_tfo_heuristics_disable:1,
        tcpi_tfo_send_blackhole:1,
        tcpi_tfo_recv_blackhole:1,
        tcpi_tfo_onebyte_proxy:1,
        __pad2:17;
    u_int64_t  tcpi_txpackets __attribute__((aligned(8)));
    u_int64_t  tcpi_txbytes __attribute__((aligned(8)));
    u_int64_t  tcpi_txretransmitbytes __attribute__((aligned(8)));
    u_int64_t  tcpi_rxpackets __attribute__((aligned(8)));
    u_int64_t  tcpi_rxbytes __attribute__((aligned(8)));
    u_int64_t  tcpi_rxoutoforderbytes __attribute__((aligned(8)));
    u_int64_t  tcpi_txretransmitpackets __attribute__((aligned(8)));
};

#endif
