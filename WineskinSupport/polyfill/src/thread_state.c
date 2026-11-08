/*
 * thread_get_register_pointer_values (10.14), as libsyscall implements it: the thread's registers that
 * hold nonzero values, read with thread_get_state, and its stack pointer below the red zone.
 */
#include <mach/thread_state.h>

kern_return_t thread_get_register_pointer_values(thread_t thread, uintptr_t *sp, size_t *length, uintptr_t *values)
{
    size_t in_length, out_length = 0;
    kern_return_t ret;

    if (!length) return KERN_INVALID_ARGUMENT;
    if (*length > 0 && !values) return KERN_INVALID_ARGUMENT;
    in_length = *length;

#define push_register_value(p) do { \
        if ((uintptr_t)(p) > 0) { \
            if (out_length < in_length) values[out_length] = (uintptr_t)(p); \
            out_length++; \
        } \
    } while (0)

#if defined(__x86_64__)
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if ((ret = thread_get_state(thread, x86_THREAD_STATE64, (thread_state_t)&state, &count)) != KERN_SUCCESS)
        return ret;
    if (sp) *sp = state.__rsp - 128;  /* the red zone */
    push_register_value(state.__rip);
    push_register_value(state.__rax);
    push_register_value(state.__rbx);
    push_register_value(state.__rcx);
    push_register_value(state.__rdx);
    push_register_value(state.__rdi);
    push_register_value(state.__rsi);
    push_register_value(state.__rbp);
    push_register_value(state.__r8);
    push_register_value(state.__r9);
    push_register_value(state.__r10);
    push_register_value(state.__r11);
    push_register_value(state.__r12);
    push_register_value(state.__r13);
    push_register_value(state.__r14);
    push_register_value(state.__r15);
#elif defined(__i386__)
    i386_thread_state_t state;
    mach_msg_type_number_t count = i386_THREAD_STATE_COUNT;
    if ((ret = thread_get_state(thread, i386_THREAD_STATE, (thread_state_t)&state, &count)) != KERN_SUCCESS)
        return ret;
    if (sp) *sp = state.__esp;
    push_register_value(state.__eip);
    push_register_value(state.__eax);
    push_register_value(state.__ebx);
    push_register_value(state.__ecx);
    push_register_value(state.__edx);
    push_register_value(state.__edi);
    push_register_value(state.__esi);
    push_register_value(state.__ebp);
#endif
#undef push_register_value

    *length = out_length;
    if (in_length == 0 || out_length > in_length) return KERN_INSUFFICIENT_BUFFER_SIZE;
    return KERN_SUCCESS;
}
