/*
 * i386/_mcontext.h with the machine contexts later SDKs add: the "full" x86_64 thread state and
 * contexts (10.14.6 SDK), which carry the segment registers, and the AVX-512 state and contexts
 * (10.13 SDK). Layouts as in xnu.
 */
#include_next <i386/_mcontext.h>

#if !defined(WINESKIN_I386_MCONTEXT_H) && defined(__x86_64__) && __DARWIN_UNIX03
#define WINESKIN_I386_MCONTEXT_H

#include <mach/i386/_structs.h>

#ifndef _STRUCT_X86_THREAD_FULL_STATE64
#define _STRUCT_X86_THREAD_FULL_STATE64 struct __darwin_x86_thread_full_state64
_STRUCT_X86_THREAD_FULL_STATE64
{
	_STRUCT_X86_THREAD_STATE64	__ss64;
	__uint64_t		__ds;
	__uint64_t		__es;
	__uint64_t		__ss;
	__uint64_t		__gsbase;
};
#endif

#ifndef _STRUCT_OPMASK_REG
#define _STRUCT_OPMASK_REG struct __darwin_opmask_reg
_STRUCT_OPMASK_REG
{
	char			__opmask_reg[8];
};
#endif

#ifndef _STRUCT_YMM_REG
#define _STRUCT_YMM_REG struct __darwin_ymm_reg
_STRUCT_YMM_REG
{
	char			__ymm_reg[32];
};
#endif

#ifndef _STRUCT_ZMM_REG
#define _STRUCT_ZMM_REG struct __darwin_zmm_reg
_STRUCT_ZMM_REG
{
	char			__zmm_reg[64];
};
#endif

#ifndef _STRUCT_X86_AVX512_STATE64
#define _STRUCT_X86_AVX512_STATE64 struct __darwin_x86_avx512_state64
_STRUCT_X86_AVX512_STATE64
{
	int			__fpu_reserved[2];
	_STRUCT_FP_CONTROL	__fpu_fcw;
	_STRUCT_FP_STATUS	__fpu_fsw;
	__uint8_t		__fpu_ftw;
	__uint8_t		__fpu_rsrv1;
	__uint16_t		__fpu_fop;
	__uint32_t		__fpu_ip;
	__uint16_t		__fpu_cs;
	__uint16_t		__fpu_rsrv2;
	__uint32_t		__fpu_dp;
	__uint16_t		__fpu_ds;
	__uint16_t		__fpu_rsrv3;
	__uint32_t		__fpu_mxcsr;
	__uint32_t		__fpu_mxcsrmask;
	_STRUCT_MMST_REG	__fpu_stmm0;
	_STRUCT_MMST_REG	__fpu_stmm1;
	_STRUCT_MMST_REG	__fpu_stmm2;
	_STRUCT_MMST_REG	__fpu_stmm3;
	_STRUCT_MMST_REG	__fpu_stmm4;
	_STRUCT_MMST_REG	__fpu_stmm5;
	_STRUCT_MMST_REG	__fpu_stmm6;
	_STRUCT_MMST_REG	__fpu_stmm7;
	_STRUCT_XMM_REG	__fpu_xmm0;
	_STRUCT_XMM_REG	__fpu_xmm1;
	_STRUCT_XMM_REG	__fpu_xmm2;
	_STRUCT_XMM_REG	__fpu_xmm3;
	_STRUCT_XMM_REG	__fpu_xmm4;
	_STRUCT_XMM_REG	__fpu_xmm5;
	_STRUCT_XMM_REG	__fpu_xmm6;
	_STRUCT_XMM_REG	__fpu_xmm7;
	_STRUCT_XMM_REG	__fpu_xmm8;
	_STRUCT_XMM_REG	__fpu_xmm9;
	_STRUCT_XMM_REG	__fpu_xmm10;
	_STRUCT_XMM_REG	__fpu_xmm11;
	_STRUCT_XMM_REG	__fpu_xmm12;
	_STRUCT_XMM_REG	__fpu_xmm13;
	_STRUCT_XMM_REG	__fpu_xmm14;
	_STRUCT_XMM_REG	__fpu_xmm15;
	char			__fpu_rsrv4[6*16];
	int			__fpu_reserved1;
	char			__avx_reserved1[64];
	_STRUCT_XMM_REG	__fpu_ymmh0;
	_STRUCT_XMM_REG	__fpu_ymmh1;
	_STRUCT_XMM_REG	__fpu_ymmh2;
	_STRUCT_XMM_REG	__fpu_ymmh3;
	_STRUCT_XMM_REG	__fpu_ymmh4;
	_STRUCT_XMM_REG	__fpu_ymmh5;
	_STRUCT_XMM_REG	__fpu_ymmh6;
	_STRUCT_XMM_REG	__fpu_ymmh7;
	_STRUCT_XMM_REG	__fpu_ymmh8;
	_STRUCT_XMM_REG	__fpu_ymmh9;
	_STRUCT_XMM_REG	__fpu_ymmh10;
	_STRUCT_XMM_REG	__fpu_ymmh11;
	_STRUCT_XMM_REG	__fpu_ymmh12;
	_STRUCT_XMM_REG	__fpu_ymmh13;
	_STRUCT_XMM_REG	__fpu_ymmh14;
	_STRUCT_XMM_REG	__fpu_ymmh15;
	_STRUCT_OPMASK_REG	__fpu_k0;
	_STRUCT_OPMASK_REG	__fpu_k1;
	_STRUCT_OPMASK_REG	__fpu_k2;
	_STRUCT_OPMASK_REG	__fpu_k3;
	_STRUCT_OPMASK_REG	__fpu_k4;
	_STRUCT_OPMASK_REG	__fpu_k5;
	_STRUCT_OPMASK_REG	__fpu_k6;
	_STRUCT_OPMASK_REG	__fpu_k7;
	_STRUCT_YMM_REG	__fpu_zmmh0;
	_STRUCT_YMM_REG	__fpu_zmmh1;
	_STRUCT_YMM_REG	__fpu_zmmh2;
	_STRUCT_YMM_REG	__fpu_zmmh3;
	_STRUCT_YMM_REG	__fpu_zmmh4;
	_STRUCT_YMM_REG	__fpu_zmmh5;
	_STRUCT_YMM_REG	__fpu_zmmh6;
	_STRUCT_YMM_REG	__fpu_zmmh7;
	_STRUCT_YMM_REG	__fpu_zmmh8;
	_STRUCT_YMM_REG	__fpu_zmmh9;
	_STRUCT_YMM_REG	__fpu_zmmh10;
	_STRUCT_YMM_REG	__fpu_zmmh11;
	_STRUCT_YMM_REG	__fpu_zmmh12;
	_STRUCT_YMM_REG	__fpu_zmmh13;
	_STRUCT_YMM_REG	__fpu_zmmh14;
	_STRUCT_YMM_REG	__fpu_zmmh15;
	_STRUCT_ZMM_REG	__fpu_zmm16;
	_STRUCT_ZMM_REG	__fpu_zmm17;
	_STRUCT_ZMM_REG	__fpu_zmm18;
	_STRUCT_ZMM_REG	__fpu_zmm19;
	_STRUCT_ZMM_REG	__fpu_zmm20;
	_STRUCT_ZMM_REG	__fpu_zmm21;
	_STRUCT_ZMM_REG	__fpu_zmm22;
	_STRUCT_ZMM_REG	__fpu_zmm23;
	_STRUCT_ZMM_REG	__fpu_zmm24;
	_STRUCT_ZMM_REG	__fpu_zmm25;
	_STRUCT_ZMM_REG	__fpu_zmm26;
	_STRUCT_ZMM_REG	__fpu_zmm27;
	_STRUCT_ZMM_REG	__fpu_zmm28;
	_STRUCT_ZMM_REG	__fpu_zmm29;
	_STRUCT_ZMM_REG	__fpu_zmm30;
	_STRUCT_ZMM_REG	__fpu_zmm31;
};
#endif

#ifndef _STRUCT_MCONTEXT64_FULL
#define _STRUCT_MCONTEXT64_FULL struct __darwin_mcontext64_full
_STRUCT_MCONTEXT64_FULL
{
	_STRUCT_X86_EXCEPTION_STATE64	__es;
	_STRUCT_X86_THREAD_FULL_STATE64	__ss;
	_STRUCT_X86_FLOAT_STATE64	__fs;
};
#endif

#ifndef _STRUCT_MCONTEXT_AVX64_FULL
#define _STRUCT_MCONTEXT_AVX64_FULL struct __darwin_mcontext_avx64_full
_STRUCT_MCONTEXT_AVX64_FULL
{
	_STRUCT_X86_EXCEPTION_STATE64	__es;
	_STRUCT_X86_THREAD_FULL_STATE64	__ss;
	_STRUCT_X86_AVX_STATE64		__fs;
};
#endif

#ifndef _STRUCT_MCONTEXT_AVX512_64_FULL
#define _STRUCT_MCONTEXT_AVX512_64_FULL struct __darwin_mcontext_avx512_64_full
_STRUCT_MCONTEXT_AVX512_64_FULL
{
	_STRUCT_X86_EXCEPTION_STATE64	__es;
	_STRUCT_X86_THREAD_FULL_STATE64	__ss;
	_STRUCT_X86_AVX512_STATE64	__fs;
};
#endif

#endif
