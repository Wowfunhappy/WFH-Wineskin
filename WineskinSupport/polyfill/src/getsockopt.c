/*
 * getsockopt(IPPROTO_TCP, TCP_CONNECTION_INFO) (10.11), from 10.9's private TCP_INFO (0x200), which
 * carries the same connection state: its 32-bit fields, then 64-bit counters at 4-byte alignment. 10.9 has
 * no TCP Fast Open and does not count retransmitted packets, so those fields are zero. Every other
 * option is 10.9's getsockopt.
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define TCP_INFO_10_9 0x200

static uint32_t u32_at(const unsigned char *p, int off) { uint32_t v; memcpy(&v, p + off, 4); return v; }
static uint64_t u64_at(const unsigned char *p, int off) { uint64_t v; memcpy(&v, p + off, 8); return v; }

int getsockopt(int fd, int level, int name, void *value, socklen_t *len)
{
    static int (*system_getsockopt)(int, int, int, void *, socklen_t *);
    if (!system_getsockopt) system_getsockopt = (int (*)(int, int, int, void *, socklen_t *))dlsym(RTLD_NEXT, "getsockopt");

    if (level == IPPROTO_TCP && name == TCP_CONNECTION_INFO)
    {
        unsigned char info[208];
        socklen_t info_len = sizeof(info);
        struct tcp_connection_info ci;

        if (!value || !len) { errno = EFAULT; return -1; }
        if (system_getsockopt(fd, IPPROTO_TCP, TCP_INFO_10_9, info, &info_len)) return -1;
        if (info_len < 132) { errno = ENOPROTOOPT; return -1; }

        memset(&ci, 0, sizeof(ci));
        ci.tcpi_state             = info[0];
        ci.tcpi_options           = info[1];
        ci.tcpi_snd_wscale        = info[2];
        ci.tcpi_rcv_wscale        = info[3];
        ci.tcpi_flags             = u32_at(info, 4);
        ci.tcpi_rto               = u32_at(info, 8);
        ci.tcpi_maxseg            = u32_at(info, 12);
        ci.tcpi_rttcur            = u32_at(info, 20);
        ci.tcpi_srtt              = u32_at(info, 24);
        ci.tcpi_rttvar            = u32_at(info, 28);
        ci.tcpi_snd_ssthresh      = u32_at(info, 36);
        ci.tcpi_snd_cwnd          = u32_at(info, 40);
        ci.tcpi_rcv_wnd           = u32_at(info, 44);
        ci.tcpi_snd_wnd           = u32_at(info, 48);
        ci.tcpi_snd_sbbytes       = u32_at(info, 64);
        ci.tcpi_txpackets         = u64_at(info, 68);
        ci.tcpi_txbytes           = u64_at(info, 76);
        ci.tcpi_txretransmitbytes = u64_at(info, 84);
        ci.tcpi_rxpackets         = u64_at(info, 100);
        ci.tcpi_rxbytes           = u64_at(info, 108);
        ci.tcpi_rxoutoforderbytes = u64_at(info, 124);

        if (*len > sizeof(ci)) *len = sizeof(ci);
        memcpy(value, &ci, *len);
        return 0;
    }
    return system_getsockopt(fd, level, name, value, len);
}
