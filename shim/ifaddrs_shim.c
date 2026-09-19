/* getifaddrs()/if_nametoindex() via ioctl. Android SELinux denies netlink bind()
 * to apps (glibc getifaddrs -> EACCES) and SIOCGIFINDEX on AF_UNIX sockets
 * (glibc if_nametoindex -> 0). Same ioctls on an AF_INET socket are allowed.
 * ponytail: IPv4 only, no ifa_dstaddr; enough for .NET NetworkInterface enumeration. */
#define _GNU_SOURCE
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

struct node {
    struct ifaddrs ifa;
    struct sockaddr_in addr, mask, brd;
    struct sockaddr_ll ll;
    char name[IFNAMSIZ];
};

/* Callers (.NET PAL) expect an AF_PACKET entry per interface like glibc emits;
 * it sizes its address array as (entries - inet entries), so without it inet
 * entries overwrite interface entries. */
static struct node *mknode(const struct ifreq *req, int fd, int packet)
{
    struct node *nd = calloc(1, sizeof *nd);
    if (!nd) return NULL;
    strncpy(nd->name, req->ifr_name, IFNAMSIZ - 1);
    nd->ifa.ifa_name = nd->name;
    struct ifreq r = *req;
    if (ioctl(fd, SIOCGIFFLAGS, &r) == 0) nd->ifa.ifa_flags = (unsigned)r.ifr_flags;
    if (packet) {
        nd->ll.sll_family = AF_PACKET;
        r = *req;
        if (ioctl(fd, SIOCGIFINDEX, &r) == 0) nd->ll.sll_ifindex = r.ifr_ifindex;
        r = *req;
        if (ioctl(fd, SIOCGIFHWADDR, &r) == 0) {
            nd->ll.sll_hatype = r.ifr_hwaddr.sa_family;
            nd->ll.sll_halen = 6;
            memcpy(nd->ll.sll_addr, r.ifr_hwaddr.sa_data, 6);
        }
        nd->ifa.ifa_addr = (struct sockaddr *)&nd->ll;
        return nd;
    }
    nd->addr = *(struct sockaddr_in *)&req->ifr_addr;
    r = *req;
    if (ioctl(fd, SIOCGIFNETMASK, &r) == 0) nd->mask = *(struct sockaddr_in *)&r.ifr_netmask;
    r = *req;
    if (ioctl(fd, SIOCGIFBRDADDR, &r) == 0) nd->brd = *(struct sockaddr_in *)&r.ifr_broadaddr;
    nd->ifa.ifa_addr = (struct sockaddr *)&nd->addr;
    nd->ifa.ifa_netmask = (struct sockaddr *)&nd->mask;
    nd->ifa.ifa_broadaddr = (struct sockaddr *)&nd->brd;
    return nd;
}

int getifaddrs(struct ifaddrs **out)
{
    *out = NULL;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ifreq reqs[64];
    struct ifconf ifc = { .ifc_len = sizeof reqs, .ifc_req = reqs };
    if (ioctl(fd, SIOCGIFCONF, &ifc) < 0) { close(fd); return -1; }
    int n = ifc.ifc_len / sizeof(struct ifreq);
    struct node *prev = NULL;
    for (int i = 0; i < 2 * n; i++) {
        struct node *nd = mknode(&reqs[i / 2], fd, i % 2 == 0);
        if (!nd) break;
        if (prev) prev->ifa.ifa_next = &nd->ifa; else *out = &nd->ifa;
        prev = nd;
    }
    close(fd);
    return 0;
}

unsigned int if_nametoindex(const char *name)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    struct ifreq r = {0};
    strncpy(r.ifr_name, name, IFNAMSIZ - 1);
    unsigned int idx = ioctl(fd, SIOCGIFINDEX, &r) == 0 ? (unsigned)r.ifr_ifindex : 0;
    close(fd);
    return idx;
}

void freeifaddrs(struct ifaddrs *ifa)
{
    while (ifa) { struct ifaddrs *next = ifa->ifa_next; free(ifa); ifa = next; }
}

#ifdef TEST
#include <stdio.h>
#include <arpa/inet.h>
#include <assert.h>
int main(void)
{
    struct ifaddrs *l; assert(getifaddrs(&l) == 0); int up = 0;
    for (struct ifaddrs *p = l; p; p = p->ifa_next) {
        if (p->ifa_addr->sa_family == AF_PACKET) { printf("%s packet idx=%d hatype=%d\n", p->ifa_name, ((struct sockaddr_ll *)p->ifa_addr)->sll_ifindex, ((struct sockaddr_ll *)p->ifa_addr)->sll_hatype); continue; }
        printf("%s idx=%u %s flags=%#x\n", p->ifa_name, if_nametoindex(p->ifa_name), inet_ntoa(((struct sockaddr_in *)p->ifa_addr)->sin_addr), p->ifa_flags);
        assert(if_nametoindex(p->ifa_name) > 0);
        if ((p->ifa_flags & (IFF_UP|IFF_RUNNING)) == (IFF_UP|IFF_RUNNING) && !(p->ifa_flags & IFF_LOOPBACK)) up++;
    }
    freeifaddrs(l); assert(up > 0); puts("OK"); return 0;
}
#endif
