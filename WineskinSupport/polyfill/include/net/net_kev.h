/* The network kernel event codes later SDKs gather in net/net_kev.h; 10.9 declares the same values in
   netinet/in_var.h and netinet6/in6_var.h. */
#ifndef WINESKIN_NET_KEV_H
#define WINESKIN_NET_KEV_H

#ifndef KEV_INET_SUBCLASS
#define KEV_INET_SUBCLASS 1
#endif
#ifndef KEV_INET_NEW_ADDR
#define KEV_INET_NEW_ADDR 1
#endif
#ifndef KEV_INET_CHANGED_ADDR
#define KEV_INET_CHANGED_ADDR 2
#endif
#ifndef KEV_INET_ADDR_DELETED
#define KEV_INET_ADDR_DELETED 3
#endif
#ifndef KEV_INET_SIFDSTADDR
#define KEV_INET_SIFDSTADDR 4
#endif
#ifndef KEV_INET_SIFBRDADDR
#define KEV_INET_SIFBRDADDR 5
#endif
#ifndef KEV_INET_SIFNETMASK
#define KEV_INET_SIFNETMASK 6
#endif
#ifndef KEV_INET_ARPCOLLISION
#define KEV_INET_ARPCOLLISION 7
#endif
#ifndef KEV_INET_PORTINUSE
#define KEV_INET_PORTINUSE 8
#endif
#ifndef KEV_INET6_SUBCLASS
#define KEV_INET6_SUBCLASS 6
#endif
#ifndef KEV_INET6_NEW_USER_ADDR
#define KEV_INET6_NEW_USER_ADDR 1
#endif
#ifndef KEV_INET6_CHANGED_ADDR
#define KEV_INET6_CHANGED_ADDR 2
#endif
#ifndef KEV_INET6_ADDR_DELETED
#define KEV_INET6_ADDR_DELETED 3
#endif
#ifndef KEV_INET6_NEW_LL_ADDR
#define KEV_INET6_NEW_LL_ADDR 4
#endif
#ifndef KEV_INET6_NEW_RTADV_ADDR
#define KEV_INET6_NEW_RTADV_ADDR 5
#endif
#ifndef KEV_INET6_DEFROUTER
#define KEV_INET6_DEFROUTER 6
#endif

#endif
