/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(_TRACE_TDX_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TDX_H

#include <linux/tracepoint.h>

#include <uapi/asm/vmx.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM tdx

#ifdef CONFIG_INTEL_TDX_GUEST

TRACE_EVENT(tdg_virtualization_exception,
	    TP_PROTO(u64 rip, u32 exit_reason, u64 exit_qual,
		     u64 gpa, u32 instr_len, u32 instr_info),
	    TP_ARGS(rip, exit_reason, exit_qual, gpa, instr_len,
		    instr_info),
	    TP_STRUCT__entry(
			     __field(u64, rip)
			     __field(u64, exit_qual)
			     __field(u64, gpa)
			     __field(u32, exit_reason)
			     __field(u32, instr_len)
			     __field(u32, instr_info)
			     ),
	    TP_fast_assign(
			   __entry->rip = rip;
			   __entry->exit_qual = exit_qual;
			   __entry->gpa = gpa;
			   __entry->exit_reason = exit_reason;
			   __entry->instr_len = instr_len;
			   __entry->instr_info = instr_info;
			   ),
	    TP_printk("reason %s rip 0x%016llx len %u info 0x%08x qual 0x%016llx gpa 0x%016llx",
		      __print_symbolic(__entry->exit_reason, VMX_EXIT_REASONS),
		      __entry->rip, __entry->instr_len, __entry->instr_info,
		      __entry->exit_qual, __entry->gpa
		      )
	    );

#endif // CONFIG_INTEL_TDX_GUEST

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH asm/trace/
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE tdx
#endif /* _TRACE_TDX_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
