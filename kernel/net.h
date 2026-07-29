/* net.h - LarzOS networking (RTL8139 + ARP/IPv4/ICMP) */
#ifndef _LARZOS_NET_H
#define _LARZOS_NET_H
int  net_init(void);                 /* detect + init the NIC; 1 if present */
void net_selftest(void);             /* print MAC/IP, ping the gateway */
int  net_ping(const unsigned char ip[4], int *rtt_ms);   /* 1 on reply */
char *net_vfile(const char *path);   /* content of a virtual /net/ file (malloc'd) */
void  net_vfile_write(const char *path, const char *data, int len);   /* write to /net/ */
char *net_http_accept(void);         /* block for an HTTP request (malloc'd) */
void  net_http_reply(const char *data, int len);
int   net_http_get(const unsigned char ip[4], unsigned short port, const char *path, char *out, int outmax);
extern int g_net_up;
extern unsigned char g_mac[6];
extern unsigned char g_ip[4], g_gw[4];
#endif
