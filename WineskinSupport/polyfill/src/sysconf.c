/*
 * sysconf(_SC_PHYS_PAGES): include/unistd.h declares the name, which 10.9's sysconf does not know. The
 * physical memory size is hw.memsize, in pages. Every other name is 10.9's sysconf.
 */
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <sys/sysctl.h>

long sysconf(int name)
{
    static long (*system_sysconf)(int);

    if (name == _SC_PHYS_PAGES)
    {
        uint64_t memsize;
        size_t len = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0)) return -1;
        return (long)(memsize / (uint64_t)getpagesize());
    }
    if (!system_sysconf) system_sysconf = (long (*)(int))dlsym(RTLD_NEXT, "sysconf");
    return system_sysconf(name);
}
