// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2020 Intel Corporation */

#undef pr_fmt
#define pr_fmt(fmt)     "x86/tdx: " fmt

#include <linux/protected_guest.h>

#include <asm/tdx.h>

/* TDX Module call Leaf IDs */
#define TDINFO				1
#define TDGETVEINFO			3

static struct {
	unsigned int gpa_width;
	unsigned long attributes;
} td_info __ro_after_init;

/*
 * Wrapper for simple hypercalls that only return a success/error code.
 */
static inline u64 tdx_hypercall(u64 fn, u64 r12, u64 r13, u64 r14, u64 r15)
{
	u64 err;

	err = __tdx_hypercall(fn, r12, r13, r14, r15, NULL);

	if (err)
		pr_warn_ratelimited("TDVMCALL fn:%llx failed with err:%llx\n",
				    fn, err);

	return err;
}

/*
 * Wrapper for the semi-common case where user need single output
 * value (R11). Callers of this function does not care about the
 * hypercall error code (mainly for IN or MMIO usecase).
 */
static inline u64 tdx_hypercall_out_r11(u64 fn, u64 r12, u64 r13,
					u64 r14, u64 r15)
{
	struct tdx_hypercall_output out = {0};
	u64 err;

	err = __tdx_hypercall(fn, r12, r13, r14, r15, &out);

	if (err)
		pr_warn_ratelimited("TDVMCALL fn:%llx failed with err:%llx\n",
				    fn, err);

	return out.r11;
}

static inline bool cpuid_has_tdx_guest(void)
{
	u32 eax, sig[3];

	if (cpuid_eax(0) < TDX_CPUID_LEAF_ID)
		return false;

	cpuid_count(TDX_CPUID_LEAF_ID, 0, &eax, &sig[0], &sig[1], &sig[2]);

	return !memcmp("IntelTDX    ", sig, 12);
}

bool tdx_protected_guest_has(unsigned long flag)
{
	switch (flag) {
	case PR_GUEST_MEM_ENCRYPT:
	case PR_GUEST_MEM_ENCRYPT_ACTIVE:
	case PR_GUEST_UNROLL_STRING_IO:
	case PR_GUEST_SHARED_MAPPING_INIT:
	case PR_GUEST_TDX:
		return static_cpu_has(X86_FEATURE_TDX_GUEST);
	}

	return false;
}
EXPORT_SYMBOL_GPL(tdx_protected_guest_has);

static void tdg_get_info(void)
{
	u64 ret;
	struct tdx_module_output out = {0};

	ret = __tdx_module_call(TDINFO, 0, 0, 0, 0, &out);

	BUG_ON(ret);

	td_info.gpa_width = out.rcx & GENMASK(5, 0);
	td_info.attributes = out.rdx;
}

unsigned long tdg_get_ve_info(struct ve_info *ve)
{
	u64 ret;
	struct tdx_module_output out = {0};

	/*
	 * NMIs and machine checks are suppressed. Before this point any
	 * #VE is fatal. After this point (TDGETVEINFO call), NMIs and
	 * additional #VEs are permitted (but we don't expect them to
	 * happen unless you panic).
	 */
	ret = __tdx_module_call(TDGETVEINFO, 0, 0, 0, 0, &out);

	ve->exit_reason = out.rcx;
	ve->exit_qual   = out.rdx;
	ve->gla         = out.r8;
	ve->gpa         = out.r9;
	ve->instr_len   = out.r10 & UINT_MAX;
	ve->instr_info  = out.r10 >> 32;

	return ret;
}

int tdg_handle_virtualization_exception(struct pt_regs *regs,
		struct ve_info *ve)
{
	/*
	 * TODO: Add handler support for various #VE exit
	 * reasons. It will be added by other patches in
	 * the series.
	 */
	pr_warn("Unexpected #VE: %lld\n", ve->exit_reason);
	return -EFAULT;
}

void __init tdx_early_init(void)
{
	if (!cpuid_has_tdx_guest())
		return;

	setup_force_cpu_cap(X86_FEATURE_TDX_GUEST);

	tdg_get_info();

	pr_info("Guest is initialized\n");
}
