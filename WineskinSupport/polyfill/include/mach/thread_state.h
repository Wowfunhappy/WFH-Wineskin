/* mach/thread_state.h, a 10.14 SDK header. src/thread_state.c implements its function on 10.9. */
#ifndef _MACH_THREAD_STATE_H_
#define _MACH_THREAD_STATE_H_

#include <stdint.h>
#include <mach/mach.h>

#ifndef KERN_INSUFFICIENT_BUFFER_SIZE
#define KERN_INSUFFICIENT_BUFFER_SIZE 52
#endif

__BEGIN_DECLS
extern kern_return_t thread_get_register_pointer_values(thread_t thread, uintptr_t *sp, size_t *length,
                                                        uintptr_t *values);
__END_DECLS

#endif
