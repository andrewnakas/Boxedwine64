/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"

#include "bufferaccess.h"

#include <stdio.h>
#include <string.h>

FsOpenNode* openCpuInfo(const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
	static BString result32;
	static BString result64;
	KThread* thread = KThread::currentThread();
	bool is64Bit = thread && thread->process && thread->process->is64Bit;
	BString& result = is64Bit ? result64 : result32;
	if (result.length() == 0) {
		U32 count = Platform::getCpuCount();
#ifdef BOXEDWINE_MULTI_THREADED
		if (KSystem::cpuAffinityCountForApp != 0 && KSystem::cpuAffinityCountForApp < count) {
			count = KSystem::cpuAffinityCountForApp;
		}
#endif
		for (U32 i = 0; i < count; i++) {
			result += "processor	: "; result += i; result += "\n";
			result += "vendor_id	: GenuineIntel\n";
			if (is64Bit) {
				result += "cpu family	: 15\n";
				result += "model		: 4\n";
				result += "model name	: Intel(R) Pentium(R) 4 CPU 3.00GHz\n";
				result += "stepping	: 1\n";
			} else {
				result += "cpu family	: 15\n";
				result += "model		: 4\n";
				result += "model name	: Intel(R) Pentium(R) 4 CPU 3.00GHz\n";
				result += "stepping	: 1\n";
			}
			result += "microcode	: 0xc6\n";
			result += "cpu MHz		: "; result += Platform::getCpuFreqMHz(); result += "\n";
			result += "cache size	: 1024 KB\n";
			result += "physical id	: "; result += i; result += "\n";
			result += "siblings	: 1\n";
			result += "core id		: 0\n";
			result += "cpu cores	: 1\n";
			result += "apicid		: 6\n";
			result += "initial apicid	: 6\n";
			result += "fdiv_bug	: no\n";
			result += "f00f_bug	: no\n";
			result += "coma_bug	: no\n";
			result += "fpu		: yes\n";
			result += "fpu_exception	: yes\n";
			result += is64Bit ? "cpuid level\t: 7\n" : "cpuid level	: 22\n"; // 32-bit: unchanged until the CPU32 ISA port (M21)
			result += "wp		: yes\n";
			// Keep this list in sync with CPU64 CPUID leaves 1 and 0x80000001.
			if (is64Bit) {
				result += "flags\t\t: fpu tsc msr cx8 cmov clflush mmx fxsr sse sse2 pni ssse3 sse4_1 sse4_2 popcnt cx16 nx syscall lm lahf_lm 3dnowprefetch\n";
			} else {
				// 32-bit guests: unchanged until the CPU32 SSE3-4.2 port (M21).
				result += "flags		: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss nx pdpe1gb rdtscp lm constant_tsc arch_perfmon xtopology tsc_reliable nonstop_tsc cpuid pni pclmulqdq fma cx16 x2apic movbe popcnt tsc_deadline_timer aes f16c hypervisor lahf_lm abm 3dnowprefetch cpuid_fault pti ssbd ibrs ibpb stibp fsgsbase tsc_adjust bmi1 smep bmi2 invpcid mpx adx smap clflushopt xsaveopt arat flush_l1d arch_capabilities\n";
			}
			result += "bugs		: cpu_meltdown spectre_v1 spectre_v2 spec_store_bypass l1tf mds swapgs itlb_multihit\n";
			result += "bogomips	: "; result += Platform::getCpuFreqMHz(); result += ".00\n";
			result += "clflush size	: 64\n";
			result += "cache_alignment	: 64\n";
			result += is64Bit ? "address sizes	: 43 bits physical, 48 bits virtual\n" : "address sizes	: 32 bits physical, 32 bits virtual\n";
			result += "power management:\n";
		}
	}
	return new BufferAccess(node, flags, result);
}
