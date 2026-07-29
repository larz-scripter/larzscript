/* net.c - LarzOS networking: PCI + RTL8139 driver + a tiny ARP/IPv4/ICMP stack.
 *
 * Polled (no interrupts). Enough to detect the card, read its MAC, and ping the
 * gateway. Uses static IP config (matches QEMU's SLIRP user network). DMA works
 * because the low 1 GiB is identity-mapped, so virtual == physical for our
 * buffers.
 */
#include <stdint.h>
#include <stddef.h>
#include "net.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

/* ---- port I/O (net.c is its own translation unit) ---- */
static inline void outb(u16 p,u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8   inb(u16 p){ u8 r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p)); return r; }
static inline void outw(u16 p,u16 v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline u16  inw(u16 p){ u16 r; __asm__ volatile("inw %1,%0":"=a"(r):"Nd"(p)); return r; }
static inline void outl(u16 p,u32 v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u32  inl(u16 p){ u32 r; __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(p)); return r; }

/* from libk.c */
int   printf(const char*,...);
int   snprintf(char*,size_t,const char*,...);
void *memcpy(void*,const void*,size_t);
void *memset(void*,int,size_t);
int   memcmp(const void*,const void*,size_t);
void *malloc(size_t);
void  free(void*);
int   strcmp(const char*,const char*);
int   strncmp(const char*,const char*,size_t);

/* ---- PCI config space ---- */
static u32 pci_read(u8 bus,u8 slot,u8 fn,u8 off){
    u32 addr=(1u<<31)|((u32)bus<<16)|((u32)slot<<11)|((u32)fn<<8)|(off&0xFC);
    outl(0xCF8,addr); return inl(0xCFC);
}
static void pci_write(u8 bus,u8 slot,u8 fn,u8 off,u32 v){
    u32 addr=(1u<<31)|((u32)bus<<16)|((u32)slot<<11)|((u32)fn<<8)|(off&0xFC);
    outl(0xCF8,addr); outl(0xCFC,v);
}

/* ---- RTL8139 ---- */
int g_net_up=0;
unsigned char g_mac[6];
unsigned char g_ip[4]={10,0,2,15}, g_gw[4]={10,0,2,2};
static u16 g_io=0;
static unsigned char g_gwmac[6];
static int g_gwmac_known=0;

static unsigned char rx_buf[8192+16+1536] __attribute__((aligned(16)));
static unsigned char tx_buf[2048] __attribute__((aligned(16)));
static int tx_cur=0;
static unsigned rx_off=0;

int net_init(void){
    /* find an RTL8139 (vendor 0x10EC, device 0x8139) on bus 0 */
    int found=0; u8 bus=0,slot=0;
    for(slot=0; slot<32; slot++){
        u32 id=pci_read(0,slot,0,0x00);
        if((id&0xFFFF)==0x10EC && (id>>16)==0x8139){ found=1; break; }
    }
    if(!found) return 0;
    /* enable I/O space + bus mastering */
    u32 cmd=pci_read(0,slot,0,0x04);
    pci_write(0,slot,0,0x04, cmd | 0x5);
    /* BAR0 = I/O base */
    g_io=(u16)(pci_read(0,slot,0,0x10) & ~0x3u);
    (void)bus;

    outb(g_io+0x52, 0x00);                 /* power on */
    outb(g_io+0x37, 0x10);                 /* software reset */
    for(int i=0;i<100000 && (inb(g_io+0x37)&0x10); i++){}
    for(int i=0;i<6;i++) g_mac[i]=inb(g_io+i);

    outl(g_io+0x30, (u32)(uintptr_t)rx_buf);   /* RBSTART (phys == virt) */
    outw(g_io+0x3C, 0x0005);                   /* IMR: ROK|TOK */
    outl(g_io+0x44, 0xf | (1<<7));             /* RCR: accept all + WRAP, 8K buf */
    outb(g_io+0x37, 0x0C);                     /* CR: RE|TE */
    rx_off=0; tx_cur=0;
    g_net_up=1;
    return 1;
}

static void nic_tx(const void *frame, int len){
    if(len<60) len=60;                     /* pad to min Ethernet frame */
    memcpy(tx_buf, frame, (size_t)len);
    outl(g_io+0x20+tx_cur*4, (u32)(uintptr_t)tx_buf);   /* TSAD */
    outl(g_io+0x10+tx_cur*4, (u32)len);                 /* TSD: length, OWN=0 -> send */
    for(int i=0;i<1000000;i++) if(inl(g_io+0x10+tx_cur*4)&0x8000) break;  /* TOK */
    tx_cur=(tx_cur+1)&3;
}

/* returns payload length into *out (points into rx_buf), or 0 if none */
static int nic_rx(unsigned char **out){
    if(inb(g_io+0x37)&0x01) return 0;      /* BUFE: buffer empty */
    unsigned char *p=rx_buf+rx_off;
    u16 status=(u16)(p[0]|(p[1]<<8)); (void)status;
    u16 len=(u16)(p[2]|(p[3]<<8));         /* includes 4-byte CRC */
    *out=p+4;
    int paylen = len>=4 ? len-4 : 0;
    rx_off=(rx_off + len + 4 + 3) & ~3u;
    if(rx_off>8192) rx_off%=8192;
    outw(g_io+0x38, (u16)(rx_off-16));     /* CAPR */
    return paylen;
}

/* ---- ARP / IPv4 / ICMP ---- */
static u16 checksum(const void *data, int len){
    const u8 *p=data; u32 sum=0;
    while(len>1){ sum += (u16)(p[0]<<8 | p[1]); p+=2; len-=2; }
    if(len) sum += (u16)(p[0]<<8);
    while(sum>>16) sum=(sum&0xFFFF)+(sum>>16);
    return (u16)~sum;
}

static void eth_send(const u8 dst[6], u16 type, const u8 *payload, int plen){
    u8 f[1600];
    memcpy(f, dst, 6);
    memcpy(f+6, g_mac, 6);
    f[12]=(u8)(type>>8); f[13]=(u8)type;
    memcpy(f+14, payload, (size_t)plen);
    nic_tx(f, 14+plen);
}

static int arp_resolve(const u8 ip[4], u8 macout[6]){
    u8 bcast[6]={0xff,0xff,0xff,0xff,0xff,0xff};
    u8 pkt[28];
    pkt[0]=0;pkt[1]=1; pkt[2]=8;pkt[3]=0; pkt[4]=6; pkt[5]=4; pkt[6]=0;pkt[7]=1;  /* req */
    memcpy(pkt+8, g_mac,6); memcpy(pkt+14, g_ip,4);
    memset(pkt+18,0,6); memcpy(pkt+24, ip,4);
    eth_send(bcast, 0x0806, pkt, 28);
    for(int tries=0; tries<2000000; tries++){
        unsigned char *fr;
        int n=nic_rx(&fr);
        if(n<=0) continue;
        if(fr[-2+14-12]) {}                /* silence */
        u16 et=(u16)(fr[12]<<8|fr[13]);
        if(et==0x0806){                    /* ARP */
            u8 *a=fr+14;
            if(a[6]==0 && a[7]==2 && memcmp(a+14, ip, 4)==0){   /* reply for our target */
                memcpy(macout, a+8, 6);
                return 1;
            }
        }
    }
    return 0;
}

int net_ping(const unsigned char ip[4], int *rtt_ms){
    if(!g_net_up) return 0;
    if(rtt_ms) *rtt_ms=0;
    if(!g_gwmac_known){
        if(!arp_resolve(g_gw, g_gwmac)) return 0;
        g_gwmac_known=1;
    }
    /* build IP + ICMP echo */
    static u16 seq=0; seq++;
    u8 icmp[8+32];
    icmp[0]=8; icmp[1]=0; icmp[2]=0; icmp[3]=0;                 /* echo request */
    icmp[4]=0x12; icmp[5]=0x34; icmp[6]=(u8)(seq>>8); icmp[7]=(u8)seq;
    for(int i=0;i<32;i++) icmp[8+i]=(u8)('a'+i%26);
    u16 ic=checksum(icmp, sizeof icmp); icmp[2]=(u8)(ic>>8); icmp[3]=(u8)ic;

    u8 iphdr[20];
    iphdr[0]=0x45; iphdr[1]=0; u16 total=20+sizeof icmp;
    iphdr[2]=(u8)(total>>8); iphdr[3]=(u8)total;
    iphdr[4]=0;iphdr[5]=1; iphdr[6]=0x40;iphdr[7]=0;           /* id, no frag */
    iphdr[8]=64; iphdr[9]=1;                                    /* ttl, proto ICMP */
    iphdr[10]=0;iphdr[11]=0;
    memcpy(iphdr+12, g_ip, 4); memcpy(iphdr+16, ip, 4);
    u16 ipc=checksum(iphdr,20); iphdr[10]=(u8)(ipc>>8); iphdr[11]=(u8)ipc;

    u8 pkt[20+8+32];
    memcpy(pkt, iphdr, 20); memcpy(pkt+20, icmp, sizeof icmp);
    eth_send(g_gwmac, 0x0800, pkt, (int)sizeof pkt);

    for(int tries=0; tries<3000000; tries++){
        unsigned char *fr;
        int n=nic_rx(&fr);
        if(n<=0) continue;
        u16 et=(u16)(fr[12]<<8|fr[13]);
        if(et!=0x0800) continue;
        u8 *ipp=fr+14;
        if((ipp[0]>>4)!=4 || ipp[9]!=1) continue;              /* IPv4 ICMP */
        int ihl=(ipp[0]&0x0F)*4;
        u8 *ic2=ipp+ihl;
        if(ic2[0]==0 && memcmp(ipp+12, ip, 4)==0){             /* echo reply from target */
            return 1;
        }
    }
    return 0;
}

/* ---- minimal single-connection TCP server (HTTP on port 80) ---- */
static void put32(u8 *p, u32 v){ p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; }
static u32  get32(const u8 *p){ return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }
static u16  tcp_cksum(const u8 *src, const u8 *dst, const u8 *tcp, int len){
    u32 sum=0;
    sum += (src[0]<<8)|src[1]; sum += (src[2]<<8)|src[3];
    sum += (dst[0]<<8)|dst[1]; sum += (dst[2]<<8)|dst[3];
    sum += 6; sum += (u16)len;
    for(int i=0;i+1<len;i+=2) sum += (tcp[i]<<8)|tcp[i+1];
    if(len&1) sum += tcp[len-1]<<8;
    while(sum>>16) sum=(sum&0xFFFF)+(sum>>16);
    return (u16)~sum;
}
static struct { int active; u8 mac[6], ip[4]; u16 port, myport; u32 our_seq, their_seq; } conn;

static void tcp_send(u8 flags, const u8 *data, int dlen){
    static u8 pkt[20+20+1460];
    u8 *ip=pkt, *tcp=pkt+20;
    tcp[0]=(u8)(conn.myport>>8); tcp[1]=(u8)conn.myport;  /* our source port */
    tcp[2]=(u8)(conn.port>>8); tcp[3]=(u8)conn.port;
    put32(tcp+4, conn.our_seq); put32(tcp+8, conn.their_seq);
    tcp[12]=0x50; tcp[13]=flags; tcp[14]=0x20; tcp[15]=0x00;  /* offset 5, window 8192 */
    tcp[16]=0;tcp[17]=0; tcp[18]=0;tcp[19]=0;
    if(dlen>1460) dlen=1460;
    if(dlen) memcpy(tcp+20, data, (size_t)dlen);
    int tcplen=20+dlen;
    u16 ck=tcp_cksum(g_ip, conn.ip, tcp, tcplen); tcp[16]=(u8)(ck>>8); tcp[17]=(u8)ck;
    u16 total=(u16)(20+tcplen);
    ip[0]=0x45;ip[1]=0; ip[2]=(u8)(total>>8);ip[3]=(u8)total;
    ip[4]=0;ip[5]=0; ip[6]=0x40;ip[7]=0; ip[8]=64; ip[9]=6; ip[10]=0;ip[11]=0;
    memcpy(ip+12,g_ip,4); memcpy(ip+16,conn.ip,4);
    u16 ic=checksum(ip,20); ip[10]=(u8)(ic>>8); ip[11]=(u8)ic;
    eth_send(conn.mac, 0x0800, pkt, total);
}

char *net_http_accept(void){                 /* blocks; returns malloc'd request */
    if(!g_net_up) return 0;
    for(long tries=0; tries<4000000000L; tries++){
        unsigned char *fr; int n=nic_rx(&fr);
        if(n<=0) continue;
        u16 et=(u16)(fr[12]<<8|fr[13]);
        if(et==0x0806){                      /* ARP: reply to who-has-our-IP */
            u8 *a=fr+14;
            if(a[6]==0 && a[7]==1 && memcmp(a+24,g_ip,4)==0){
                u8 rep[28]; memcpy(rep,a,28); rep[7]=2;
                memcpy(rep+8,g_mac,6); memcpy(rep+14,g_ip,4);
                memcpy(rep+18,a+8,6); memcpy(rep+24,a+14,4);
                eth_send(a+8, 0x0806, rep, 28);
            }
            continue;
        }
        if(et!=0x0800) continue;
        u8 *ip=fr+14; if(ip[9]!=6) continue;
        int ihl=(ip[0]&0x0F)*4; u8 *tcp=ip+ihl;
        if(((tcp[2]<<8)|tcp[3])!=80) continue;
        u16 sport=(u16)(tcp[0]<<8|tcp[1]);
        u32 seq=get32(tcp+4); u8 flags=tcp[13];
        int tcphl=(tcp[12]>>4)*4;
        int plen=((ip[2]<<8)|ip[3]) - ihl - tcphl;
        if(flags & 0x02){                    /* SYN -> SYN,ACK */
            memcpy(conn.mac, fr+6, 6); memcpy(conn.ip, ip+12, 4);
            conn.port=sport; conn.myport=80; conn.their_seq=seq+1; conn.our_seq=12345; conn.active=1;
            tcp_send(0x12, 0, 0); conn.our_seq++;
            continue;
        }
        if(conn.active && plen>0){           /* data: the HTTP request */
            conn.their_seq = seq + plen;
            tcp_send(0x10, 0, 0);            /* ACK the request */
            char *req=malloc((size_t)plen+1); if(!req) return 0;
            memcpy(req, tcp+tcphl, (size_t)plen); req[plen]=0;
            return req;
        }
    }
    return 0;
}
void net_http_reply(const char *data, int len){
    if(!conn.active) return;
    tcp_send(0x18|0x01, (const u8*)data, len);       /* PSH|ACK|FIN */
    conn.our_seq += (u32)len + 1;
    for(int i=0;i<800000;i++){                        /* ACK the client's FIN */
        unsigned char *fr; int n=nic_rx(&fr);
        if(n<=0) continue;
        u16 et=(u16)(fr[12]<<8|fr[13]); if(et!=0x0800) continue;
        u8 *ip=fr+14; if(ip[9]!=6) continue;
        int ihl=(ip[0]&0x0F)*4; u8 *tcp=ip+ihl;
        if(((tcp[2]<<8)|tcp[3])!=80) continue;
        if(tcp[13]&0x01){ conn.their_seq=get32(tcp+4)+1; tcp_send(0x10,0,0); break; }
    }
    conn.active=0;
}

/* ---- outbound TCP: a tiny HTTP client (active open) ---- */
static int tcp_from_server(unsigned char *fr, int *plen, u8 *flags, u32 *seq, u8 **pay){
    u16 et=(u16)(fr[12]<<8|fr[13]); if(et!=0x0800) return 0;
    u8 *ip=fr+14; if(ip[9]!=6) return 0;
    if(memcmp(ip+12, conn.ip, 4)!=0) return 0;
    int ihl=(ip[0]&0x0F)*4; u8 *tcp=ip+ihl;
    if(((tcp[0]<<8)|tcp[1])!=conn.port || ((tcp[2]<<8)|tcp[3])!=conn.myport) return 0;
    int tcphl=(tcp[12]>>4)*4;
    *plen=((ip[2]<<8)|ip[3]) - ihl - tcphl; *flags=tcp[13]; *seq=get32(tcp+4); *pay=tcp+tcphl;
    return 1;
}
static u16 g_eport=49152;
int net_http_get(const unsigned char dstip[4], u16 port, const char *path, char *out, int outmax){
    if(!g_net_up) return -1;
    if(!g_gwmac_known){ if(!arp_resolve(g_gw,g_gwmac)) return -1; g_gwmac_known=1; }
    { unsigned char *fr; int guard=200000; while(guard-->0 && nic_rx(&fr)>0){} }  /* drain stale RX */
    memcpy(conn.mac,g_gwmac,6); memcpy(conn.ip,dstip,4);
    if(++g_eport < 49152) g_eport=49152;                    /* fresh 4-tuple each connection */
    conn.port=port; conn.myport=g_eport; conn.our_seq=20000+(u32)g_eport*131; conn.their_seq=0; conn.active=1;
    tcp_send(0x02,0,0); conn.our_seq++;                 /* SYN */
    int plen; u8 fl; u32 seq; u8 *pay; int got=0;
    for(int i=0;i<6000000 && !got;i++){
        unsigned char *fr; if(nic_rx(&fr)<=0) continue;
        if(!tcp_from_server(fr,&plen,&fl,&seq,&pay)) continue;
        if((fl&0x12)==0x12){ conn.their_seq=seq+1; tcp_send(0x10,0,0); got=1; }
        else if(fl&0x04){ conn.active=0; return -1; }
    }
    if(!got){ conn.active=0; return -1; }
    char req[300]; int rl=0;
    for(const char *p="GET ";*p;p++) req[rl++]=*p;
    for(const char *p=path;*p;p++) req[rl++]=*p;
    for(const char *p=" HTTP/1.0\r\nHost: larzos\r\nConnection: close\r\n\r\n";*p;p++) req[rl++]=*p;
    tcp_send(0x18,(u8*)req,rl); conn.our_seq += (u32)rl;
    int total=0, done=0;
    for(int i=0;i<40000000 && !done;i++){
        unsigned char *fr; if(nic_rx(&fr)<=0) continue;
        if(!tcp_from_server(fr,&plen,&fl,&seq,&pay)) continue;
        if(plen>0 && seq==conn.their_seq){
            int cp=plen; if(total+cp>outmax) cp=outmax-total;
            if(cp>0){ memcpy(out+total, pay, (size_t)cp); total+=cp; }
            conn.their_seq += (u32)plen; tcp_send(0x10,0,0);
        } else if(plen>0){ tcp_send(0x10,0,0); }     /* out of order: re-ack */
        if(fl&0x01){ conn.their_seq++; tcp_send(0x10,0,0); done=1; }
    }
    conn.active=0;
    return total;
}

void net_selftest(void){
    if(!net_init()){ printf("  net: no NIC found (add -device rtl8139)\n"); return; }
    printf("  net: RTL8139 up  MAC %02x:%02x:%02x:%02x:%02x:%02x  IP %d.%d.%d.%d  GW %d.%d.%d.%d\n",
        g_mac[0],g_mac[1],g_mac[2],g_mac[3],g_mac[4],g_mac[5],
        g_ip[0],g_ip[1],g_ip[2],g_ip[3], g_gw[0],g_gw[1],g_gw[2],g_gw[3]);
    int rtt;
    if(net_ping(g_gw, &rtt)) printf("  net: ping %d.%d.%d.%d -> reply OK\n", g_gw[0],g_gw[1],g_gw[2],g_gw[3]);
    else printf("  net: ping %d.%d.%d.%d -> no reply\n", g_gw[0],g_gw[1],g_gw[2],g_gw[3]);
}

/* ---- virtual /net files: expose networking to Larzscript via read_file ---- */
static int parse_ip(const char *s, unsigned char ip[4]){
    int o=0,v=0,seen=0;
    for(const char *p=s;;p++){
        if(*p>='0'&&*p<='9'){ v=v*10+(*p-'0'); seen=1; }
        else if(*p=='.'||*p==0||*p=='\n'){ if(!seen||o>3) return 0; ip[o++]=(unsigned char)v; v=0; seen=0; if(*p==0||*p=='\n') break; }
        else break;
    }
    return o==4;
}
char *net_vfile(const char *path){          /* returns malloc'd content, or 0 */
    if(strcmp(path,"/net/http/accept")==0) return g_net_up ? net_http_accept() : 0;
    if(strncmp(path,"/net/get/",9)==0){       /* HTTP GET: /net/get/<ip[:port]>/<path> */
        const char *r=path+9; unsigned char ip[4]={0,0,0,0}; int o=0,v=0;
        const char *p=r;
        for(; *p && *p!=':' && *p!='/'; p++){ if(*p=='.'){ if(o<3) ip[o++]=(u8)v; v=0; } else if(*p>='0'&&*p<='9') v=v*10+(*p-'0'); }
        if(o<4) ip[o++]=(u8)v;
        int port=80; if(*p==':'){ p++; port=0; for(; *p && *p!='/'; p++) if(*p>='0'&&*p<='9') port=port*10+(*p-'0'); }
        const char *hp = (*p=='/') ? p : "/";
        char *resp=malloc(65536); if(!resp) return 0;
        int len=net_http_get(ip,(u16)port,hp,resp,65535);
        if(len<0){ free(resp); char *e=malloc(8); if(e) { e[0]='E';e[1]='R';e[2]='R';e[3]='\n';e[4]=0; } return e; }
        resp[len]=0;
        char *body=resp;
        for(int i=0;i+3<len;i++) if(resp[i]=='\r'&&resp[i+1]=='\n'&&resp[i+2]=='\r'&&resp[i+3]=='\n'){ body=resp+i+4; break; }
        int bl=0; while(body[bl]) bl++;
        char *out=malloc((size_t)bl+1); if(out){ memcpy(out,body,(size_t)bl+1); }
        free(resp); return out;
    }
    char *b=malloc(256); if(!b) return 0;
    if(strcmp(path,"/net/status")==0){
        if(!g_net_up){ snprintf(b,256,"link: down (no NIC)\n"); return b; }
        snprintf(b,256,"link: up\nmac: %02x:%02x:%02x:%02x:%02x:%02x\nip: %d.%d.%d.%d\ngw: %d.%d.%d.%d\n",
            g_mac[0],g_mac[1],g_mac[2],g_mac[3],g_mac[4],g_mac[5],
            g_ip[0],g_ip[1],g_ip[2],g_ip[3], g_gw[0],g_gw[1],g_gw[2],g_gw[3]);
        return b;
    }
    if(strncmp(path,"/net/ping/",10)==0){
        if(!g_net_up){ snprintf(b,256,"no network\n"); return b; }
        unsigned char ip[4];
        if(!parse_ip(path+10, ip)){ snprintf(b,256,"bad address\n"); return b; }
        int rtt; snprintf(b,256, net_ping(ip,&rtt) ? "reply\n" : "timeout\n");
        return b;
    }
    snprintf(b,256,"unknown /net file\n"); return b;
}
void net_vfile_write(const char *path, const char *data, int len){
    if(strcmp(path,"/net/http/reply")==0) net_http_reply(data, len);
}
