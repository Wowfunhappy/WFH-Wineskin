/* mach/thread_info.h with the thread name length later SDKs declare here. */
#include_next <mach/thread_info.h>

#ifndef MAXTHREADNAMESIZE
#define MAXTHREADNAMESIZE 64
#endif
