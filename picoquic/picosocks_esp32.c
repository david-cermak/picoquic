/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* ESP32-specific version: IPv4 only, no IPv6 support
*/

#include "picosocks.h"
#include "picoquic_utils.h"

int picoquic_bind_to_port(SOCKET_TYPE fd, int af, int port)
{
    struct sockaddr_storage sa;
    int addr_length = 0;

    /* ESP-IDF only supports IPv4 */
    if (af != AF_INET) {
        return -1;
    }

    memset(&sa, 0, sizeof(sa));

    struct sockaddr_in* s4 = (struct sockaddr_in*)&sa;
    s4->sin_family = af;
    s4->sin_port = htons((unsigned short)port);
    addr_length = sizeof(struct sockaddr_in);

    return bind(fd, (struct sockaddr*)&sa, addr_length);
}

int picoquic_get_local_address(SOCKET_TYPE sd, struct sockaddr_storage * addr)
{
    socklen_t name_len = sizeof(struct sockaddr_storage);
    return getsockname(sd, (struct sockaddr *)addr, &name_len);
}

int picoquic_socket_set_pkt_info(SOCKET_TYPE sd, int af)
{
    int ret = -1;

    /* ESP-IDF lwIP doesn't support IPv6 packet info, only IPv4 */
    if (af != AF_INET) {
        return -1;
    }

    int val = 1;
#ifdef IP_PKTINFO
    ret = setsockopt(sd, IPPROTO_IP, IP_PKTINFO, (char*)&val, sizeof(int));
#else
    /* The IP_PKTINFO structure is not defined on BSD */
    ret = setsockopt(sd, IPPROTO_IP, IP_RECVDSTADDR, (char*)&val, sizeof(int));
#endif

    return ret;
}

int picoquic_socket_set_ecn_options(SOCKET_TYPE sd, int af, int * recv_set, int * send_set)
{
    int ret = -1;

    /* ESP-IDF lwIP doesn't support IPv6 ECN options, skip IPv6 */
    if (af != AF_INET) {
        *recv_set = 0;
        *send_set = 0;
        return 0;
    }

    if (af == AF_INET) {
#if defined(IP_TOS)
        {
            unsigned int ecn = PICOQUIC_ECN_ECT_1;
            /* Request setting ECN_ECT_1 in outgoing packets */
            if (setsockopt(sd, IPPROTO_IP, IP_TOS, &ecn, sizeof(ecn)) < 0) {
                DBG_PRINTF("setsockopt IPv4 IP_TOS (0x%x) fails, errno: %d\n", ecn, errno);
                *send_set = 0;
            }
            else {
                *send_set = 1;
            }
        }
#else
        *send_set = 0;
        DBG_PRINTF("%s", "IP_TOS is not defined\n");
#endif

#ifdef IP_RECVTOS
        {
            unsigned int set = 1;

            /* Request receiving TOS reports in recvmsg */
            if (setsockopt(sd, IPPROTO_IP, IP_RECVTOS, &set, sizeof(set)) < 0) {
                DBG_PRINTF("setsockopt IPv4 IP_RECVTOS (0x%x) fails, errno: %d\n", set, errno);
                ret = -1;
                *recv_set = 0;
            }
            else {
                *recv_set = 1;
                ret = 0;
            }
        }
#else
        *recv_set = 0;
        DBG_PRINTF("%s", "IP_RECVTOS is not defined\n");
#endif
    }

    return ret;
}

int picoquic_socket_set_pmtud_options(SOCKET_TYPE sd, int af)
{
    int ret = 0;
#if defined __linux && defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_PROBE)
    if (af == AF_INET) {
        int val = IP_PMTUDISC_PROBE;
        ret = setsockopt(sd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(int));
    }
#else
    (void)af;
    (void)sd;
#endif
    return ret;
}

SOCKET_TYPE picoquic_open_client_socket(int af)
{
    /* ESP-IDF only supports IPv4 */
    if (af != AF_INET) {
        return INVALID_SOCKET;
    }

    SOCKET_TYPE sd = socket(af, SOCK_DGRAM, IPPROTO_UDP);

    if (sd != INVALID_SOCKET) {
        int send_set = 0;
        int recv_set = 0;

        if (picoquic_socket_set_pkt_info(sd, af) != 0) {
            DBG_PRINTF("Cannot set PKTINFO option (af=%d)\n", af);
        }
        if (picoquic_socket_set_ecn_options(sd, af, &recv_set, &send_set) != 0) {
            DBG_PRINTF("Cannot set ECN options (af=%d)\n", af);
        }
        if (picoquic_socket_set_pmtud_options(sd, af) != 0) {
            DBG_PRINTF("Cannot set PMTUD options (af=%d)\n", af);
        }
    }
    else {
        DBG_PRINTF("Cannot open socket(AF=%d), error: %d\n", af, errno);
    }

    return sd;
}

int picoquic_open_server_sockets(picoquic_server_sockets_t* sockets, int port)
{
    int ret = 0;

    /* ESP-IDF lwIP doesn't support IPv6 packet info, use IPv4 only */
    const int sock_af[] = { AF_INET };
    const int nb_sockets = 1;

    for (int i = 0; i < nb_sockets; i++) {
        if (ret == 0) {
            sockets->s_socket[i] = socket(sock_af[i], SOCK_DGRAM, IPPROTO_UDP);
        } else {
            sockets->s_socket[i] = INVALID_SOCKET;
        }

        if (sockets->s_socket[i] == INVALID_SOCKET) {
            ret = -1;
        }
        else {
            int recv_set = 0;
            int send_set = 0;
            if (picoquic_socket_set_ecn_options(sockets->s_socket[i], sock_af[i], &recv_set, &send_set) != 0) {
                DBG_PRINTF("Cannot set ECN options (af=%d)\n", sock_af[i]);
            }
            ret = picoquic_socket_set_pkt_info(sockets->s_socket[i], sock_af[i]);
            if (ret == 0) {
                ret = picoquic_bind_to_port(sockets->s_socket[i], sock_af[i], port);
            }
            if (ret == 0) {
                ret = picoquic_socket_set_pmtud_options(sockets->s_socket[i], sock_af[i]);
            }
        }
    }

    return ret;
}

void picoquic_close_server_sockets(picoquic_server_sockets_t* sockets)
{
    for (int i = 0; i < PICOQUIC_NB_SERVER_SOCKETS; i++) {
        if (sockets->s_socket[i] != INVALID_SOCKET) {
            SOCKET_CLOSE(sockets->s_socket[i]);
            sockets->s_socket[i] = INVALID_SOCKET;
        }
    }
}

void picoquic_socks_cmsg_parse(
    void* vmsg,
    struct sockaddr_storage* addr_dest,
    int* dest_if,
    unsigned char* received_ecn,
    size_t * udp_coalesced_size)
{
    /* Assume that msg has been filled by a call to recvmsg */
    struct msghdr* msg = (struct msghdr*)vmsg;
    struct cmsghdr* cmsg;

    for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP) {
#ifdef IP_PKTINFO
            if (cmsg->cmsg_type == IP_PKTINFO) {
                if (addr_dest != NULL) {
                    struct in_pktinfo* pPktInfo = (struct in_pktinfo*)CMSG_DATA(cmsg);
                    ((struct sockaddr_in*)addr_dest)->sin_family = AF_INET;
                    ((struct sockaddr_in*)addr_dest)->sin_port = 0;
                    ((struct sockaddr_in*)addr_dest)->sin_addr.s_addr = pPktInfo->ipi_addr.s_addr;

                    if (dest_if != NULL) {
                        *dest_if = (int)pPktInfo->ipi_ifindex;
                    }
                }
            }
#else
            /* The IP_PKTINFO structure is not defined on BSD */
            if (cmsg->cmsg_type == IP_RECVDSTADDR) {
                if (addr_dest != NULL) {
                    struct in_addr* pPktInfo = (struct in_addr*)CMSG_DATA(cmsg);
                    ((struct sockaddr_in*)addr_dest)->sin_family = AF_INET;
                    ((struct sockaddr_in*)addr_dest)->sin_port = 0;
                    ((struct sockaddr_in*)addr_dest)->sin_addr.s_addr = pPktInfo->s_addr;

                    if (dest_if != NULL) {
                        *dest_if = 0;
                    }
                }
            }
#endif
            else if ((cmsg->cmsg_type == IP_TOS
#ifdef IP_RECVTOS
                || cmsg->cmsg_type == IP_RECVTOS
#endif
                ) && cmsg->cmsg_len > 0) {
                if (received_ecn != NULL) {
                    *received_ecn = *((unsigned char*)CMSG_DATA(cmsg));
                }
            }
        }
    }
}

static void* cmsg_format_header_return_data_ptr(struct msghdr* msg, struct cmsghdr** last_cmsg, int* control_length,
    int cmsg_level, int cmsg_type, size_t cmsg_data_len)
{
    void* cmsg_data_ptr = NULL;
#ifdef CMSG_ALIGN
    struct cmsghdr* cmsg = (*last_cmsg == NULL) ? CMSG_FIRSTHDR(msg) :
        (struct cmsghdr*)((unsigned char*)(*last_cmsg) + CMSG_ALIGN((*last_cmsg)->cmsg_len));
#else
    struct cmsghdr* cmsg = (*last_cmsg == NULL) ? CMSG_FIRSTHDR(msg) : CMSG_NXTHDR(msg, *last_cmsg);
#endif

    if (cmsg != NULL) {
        size_t cmsg_required_space = CMSG_SPACE(cmsg_data_len);
        *control_length += (int)cmsg_required_space;
        memset(cmsg, 0, cmsg_required_space);
        cmsg->cmsg_level = cmsg_level;
        cmsg->cmsg_type = cmsg_type;
        cmsg->cmsg_len = CMSG_LEN(cmsg_data_len);
        cmsg_data_ptr = (void*)CMSG_DATA(cmsg);
        *last_cmsg = cmsg;
    }

    return cmsg_data_ptr;
}

void picoquic_socks_cmsg_format(
    void* vmsg,
    size_t message_length,
    size_t send_msg_size,
    struct sockaddr* addr_from,
    int dest_if)
{
    struct msghdr* msg = (struct msghdr*)vmsg;
    int control_length = 0;
    struct cmsghdr* last_cmsg = NULL;
    int is_null = 0;

    if (addr_from != NULL && addr_from->sa_family != 0) {
        if (addr_from->sa_family == AF_INET) {
#ifdef IP_PKTINFO
            struct in_pktinfo* pktinfo = (struct in_pktinfo*)cmsg_format_header_return_data_ptr(msg, &last_cmsg,
                &control_length, IPPROTO_IP, IP_PKTINFO, sizeof(struct in_pktinfo));
            if (pktinfo != NULL) {
                /* ESP-IDF lwIP uses ipi_addr instead of ipi_spec_dst */
                pktinfo->ipi_addr.s_addr = ((struct sockaddr_in*)addr_from)->sin_addr.s_addr;
                pktinfo->ipi_ifindex = (unsigned long)dest_if;
            }
            else {
                is_null = 1;
            }
#else 
            /* The IP_PKTINFO structure is not defined on BSD */
            /* Some versions of freeBSD do not define IP_SENDSRCADDR, use IP_RECVDSTADDR instead. */
            struct in_addr* pktinfo = (struct in_addr*)cmsg_format_header_return_data_ptr(msg, &last_cmsg,
                &control_length, IPPROTO_IP,
#ifdef IP_SENDSRCADDR
                IP_SENDSRCADDR
#else
                IP_RECVDSTADDR
#endif
                , sizeof(struct in_addr));
            if (pktinfo != NULL) {
                pktinfo->s_addr = ((struct sockaddr_in*)addr_from)->sin_addr.s_addr;
            }
            else {
                is_null = 1;
            }
#endif
        }
    }
#if defined(UDP_SEGMENT)
    if (!is_null && send_msg_size > 0 && send_msg_size < message_length) {
        uint16_t* pval = (uint16_t*)cmsg_format_header_return_data_ptr(msg, &last_cmsg,
            &control_length, SOL_UDP, UDP_SEGMENT, sizeof(uint16_t));
        if (pval != NULL) {
            *pval = (uint16_t)send_msg_size;
        }
        else {
            is_null = 1;
        }
    }
#endif

    msg->msg_controllen = control_length;
    if (control_length == 0) {
        msg->msg_control = NULL;
    }
}

int picoquic_recvmsg(SOCKET_TYPE fd,
    struct sockaddr_storage* addr_from,
    struct sockaddr_storage* addr_dest,
    int* dest_if,
    unsigned char* received_ecn,
    uint8_t* buffer, int buffer_max)
{
    int bytes_recv = 0;
    struct msghdr msg;
    struct iovec dataBuf;
    char cmsg_buffer[1024];

    if (dest_if != NULL) {
        *dest_if = 0;
    }

    dataBuf.iov_base = (char*)buffer;
    dataBuf.iov_len = buffer_max;

    msg.msg_name = (struct sockaddr*)addr_from;
    msg.msg_namelen = sizeof(struct sockaddr_storage);
    msg.msg_iov = &dataBuf;
    msg.msg_iovlen = 1;
    msg.msg_flags = 0;
    msg.msg_control = (void*)cmsg_buffer;
    msg.msg_controllen = sizeof(cmsg_buffer);

    bytes_recv = recvmsg(fd, &msg, 0);

    if (bytes_recv <= 0) {
        addr_from->ss_family = 0;
    } else {
        picoquic_socks_cmsg_parse(&msg, addr_dest, dest_if, received_ecn, NULL);
    }

    return bytes_recv;
}

int picoquic_sendmsg(SOCKET_TYPE fd,
    struct sockaddr* addr_dest,
    struct sockaddr* addr_from,
    int dest_if,
    const char* bytes, int length,
    int send_msg_size,
    int * sock_err)
{
    struct msghdr msg;
    struct iovec dataBuf;
    char cmsg_buffer[1024];
    int bytes_sent;

    /* Format the message header */

    dataBuf.iov_base = (char*)bytes;
    dataBuf.iov_len = length;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = addr_dest;
    msg.msg_namelen = picoquic_addr_length(addr_dest);
    msg.msg_iov = &dataBuf;
    msg.msg_iovlen = 1;
    msg.msg_control = (void*)cmsg_buffer;
    msg.msg_controllen = sizeof(cmsg_buffer);

    /* Format the control message */
    picoquic_socks_cmsg_format(&msg, length, send_msg_size, addr_from, dest_if);

    bytes_sent = sendmsg(fd, &msg, 0);

    if (bytes_sent <= 0) {
        int last_error = errno;
#ifndef DISABLE_DEBUG_PRINTF
        DBG_PRINTF("Could not send packet on UDP socket[AF=%d]= %d!\n",
            addr_dest->sa_family, last_error);
#endif
        if (sock_err != NULL) {
            *sock_err = last_error;
        }
    }
    return bytes_sent;
}

int picoquic_select_ex(SOCKET_TYPE* sockets,
    int nb_sockets,
    struct sockaddr_storage* addr_from,
    struct sockaddr_storage* addr_dest,
    int* dest_if,
    unsigned char * received_ecn,
    uint8_t* buffer, int buffer_max,
    int64_t delta_t,
    int * socket_rank,
    uint64_t* current_time)
{
    fd_set readfds;
    struct timeval tv;
    int ret_select = 0;
    int bytes_recv = 0;
    int sockmax = 0;

    if (received_ecn != NULL) {
        *received_ecn = 0;
    }

    FD_ZERO(&readfds);

    for (int i = 0; i < nb_sockets; i++) {
        if (sockmax < (int)sockets[i]) {
            sockmax = (int)sockets[i];
        }
        FD_SET(sockets[i], &readfds);
    }

    if (delta_t <= 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    } else {
        if (delta_t > 10000000) {
            tv.tv_sec = (long)10;
            tv.tv_usec = 0;
        } else {
            tv.tv_sec = (long)(delta_t / 1000000);
            tv.tv_usec = (long)(delta_t % 1000000);
        }
    }

    ret_select = select(sockmax + 1, &readfds, NULL, NULL, &tv);

    if (ret_select < 0) {
        bytes_recv = -1;
        DBG_PRINTF("Error: select returns %d\n", ret_select);
    } else if (ret_select > 0) {
        for (int i = 0; i < nb_sockets; i++) {
            if (FD_ISSET(sockets[i], &readfds)) {
                *socket_rank = i;
                bytes_recv = picoquic_recvmsg(sockets[i], addr_from,
                    addr_dest, dest_if, received_ecn,
                    buffer, buffer_max);

                if (bytes_recv <= 0) {
                    DBG_PRINTF("Could not receive packet on UDP socket[%d]= %d!\n",
                        i, (int)sockets[i]);

                    break;
                } else {
                    break;
                }
            }
        }
    }

    *current_time = picoquic_current_time();

    return bytes_recv;
}

int picoquic_select(SOCKET_TYPE* sockets,
    int nb_sockets,
    struct sockaddr_storage* addr_from,
    struct sockaddr_storage* addr_dest,
    int* dest_if,
    unsigned char* received_ecn,
    uint8_t* buffer, int buffer_max,
    int64_t delta_t,
    uint64_t* current_time) {
    int socket_rank;
    return picoquic_select_ex(sockets, nb_sockets, addr_from, addr_dest, dest_if,
        received_ecn, buffer, buffer_max, delta_t, &socket_rank, current_time);
}

int picoquic_send_through_socket(
    SOCKET_TYPE fd,
    struct sockaddr* addr_dest,
    struct sockaddr* addr_from, int from_if,
    const char* bytes, int length, int* sock_err)
{
    int sent = picoquic_sendmsg(fd, addr_dest, addr_from, from_if, bytes, length, 0, sock_err);

    return sent;
}

int picoquic_send_through_server_sockets(
    picoquic_server_sockets_t* sockets,
    struct sockaddr* addr_dest,
    struct sockaddr* addr_from, int from_if,
    const char* bytes, int length, int* sock_err)
{
    /* ESP-IDF only supports IPv4 */
    if (addr_dest->sa_family != AF_INET) {
        if (sock_err != NULL) {
            *sock_err = EAFNOSUPPORT;
        }
        return -1;
    }
    return picoquic_send_through_socket(sockets->s_socket[0], addr_dest, addr_from, from_if, bytes, length, sock_err);
}

int picoquic_get_server_address(const char* ip_address_text, int server_port,
    struct sockaddr_storage* server_address, int* is_name)
{
    int ret = 0;
    struct sockaddr_in* ipv4_dest = (struct sockaddr_in*)server_address;

    /* get the IP address of the server */
    memset(server_address, 0, sizeof(struct sockaddr_storage));
    *is_name = 0;

    if (inet_pton(AF_INET, ip_address_text, &ipv4_dest->sin_addr) == 1) {
        /* Valid IPv4 address */
        ipv4_dest->sin_family = AF_INET;
        ipv4_dest->sin_port = htons((unsigned short)server_port);
    }
    else {
        /* Server is described by name. Do a lookup for the IP address,
        * and then use the name as SNI parameter */
        struct addrinfo* result = NULL;
        struct addrinfo hints;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;  /* ESP-IDF only supports IPv4 */
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        if ((ret = getaddrinfo(ip_address_text, NULL, &hints, &result)) != 0) {
            int err = ret;
            fprintf(stderr, "Cannot get IP address for %s, err = %d (0x%x)\n", ip_address_text, err, err);
            ret = -1;
        } else {
            *is_name = 1;

            switch (result->ai_family) {
            case AF_INET:
                ipv4_dest->sin_family = AF_INET;
                ipv4_dest->sin_port = htons((unsigned short)server_port);
                ipv4_dest->sin_addr.s_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr.s_addr;
                break;
            default:
                fprintf(stderr, "Error getting IPv4 address for %s, family = %d\n",
                    ip_address_text, result->ai_family);
                ret = -1;
                break;
            }

            freeaddrinfo(result);
        }
    }

    return ret;
}

/* Wireshark needs the session keys in order to decrypt and analyze packets.
 * In Unix and Windows, Wireshark reads these keys from a file. The name
 * of the file is passed in the environment variable SSLKEYLOGFILE,
 * which is accessed through system dependent API.
 * 
 * This is a very dangerous API, so we implement two levels of protection:
 *  * The feature can only be enabled if the build is compiled without
 *    the option "PICOQUIC_WITHOUT_SSLKEYLOG"
 *  * The feature is only enabled if the "SSLKEYLOG" option is
 *    explicitly set.
 */

void picoquic_set_key_log_file_from_env(picoquic_quic_t* quic)
{
#ifndef PICOQUIC_WITHOUT_SSLKEYLOG
    if (picoquic_is_sslkeylog_enabled(quic)) {
        char* keylog_filename = getenv("SSLKEYLOGFILE");
        if (keylog_filename == NULL) {
            return;
        }

        picoquic_set_key_log_file(quic, keylog_filename);
    }
#endif /* PICOQUIC_WITHOUT_SSLKEYLOG */
}

/* Some socket errors, but not all, indicate that a destination is
 * unreachable and that the corresponding "path" should be abandoned.
 */

int picoquic_socket_error_implies_unreachable(int sock_err)
{
    static int unreachable_errors[] = {
        EAFNOSUPPORT, ECONNRESET, EHOSTUNREACH, ENETDOWN, ENETUNREACH, -1 };
    size_t nb_errors = sizeof(unreachable_errors) / sizeof(int);
    int ret = 0;

    for (size_t i = 0; ret == 0 && i < nb_errors; i++) {
        ret = (sock_err == unreachable_errors[i]);
    }

    return ret;
}

