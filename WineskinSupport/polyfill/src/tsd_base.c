/*
 * _thread_set_tsd_base (10.12): sets the calling thread's TSD base (GS on x86_64), as libsyscall's thunk
 * does, through machine-dependent trap 3, thread_fast_set_cthread_self64, which 10.9's kernel has.
 */
#include <stdint.h>

#ifdef __x86_64__
void _thread_set_tsd_base(uint64_t tsd_base)
{
    __asm__ volatile ("syscall"
                      :: "a"(0x3000003), "D"(tsd_base), "S"(0)
                      : "rcx", "r11", "memory");
}
#endif
