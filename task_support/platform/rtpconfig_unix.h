#ifndef RTPCONFIG_UNIX_H
#define RTPCONFIG_UNIX_H
/* POSIX-only build profile, based on configure.in defaults. */
#define RTP_SOCKLENTYPE_UINT
#define RTP_SUPPORT_SDESPRIV
#define RTP_SUPPORT_INLINETEMPLATEPARAM
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define RTP_BIG_ENDIAN
#endif
#ifdef __APPLE__
#define RTP_HAVE_SOCKADDR_LEN
#endif
/* No JThread, probation, IPv6, multicast or runtime dependency discovery. */
#endif
