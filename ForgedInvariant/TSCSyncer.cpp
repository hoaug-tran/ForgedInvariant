// Copyright © 2024-2025 ChefKiss, licensed under the Thou Shalt Not Profit License version 1.5.
// See LICENSE for details.

#include "TSCSyncer.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_util.hpp>
#include <i386/proc_reg.h>
#include <IOKit/IOTimerEventSource.h>
#include <sys/types.h>
#include <stdatomic.h>

// Definitions & Constants

// Private kernel function to sync all cores (not exposed in public SDKs)
extern "C" {
	void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);
}

#ifdef MSR_CORE_THREAD_COUNT
#undef MSR_CORE_THREAD_COUNT
#endif

#ifdef MSR_TSC
#undef MSR_TSC
#endif

#ifdef MSR_TSC_ADJUST
#undef MSR_TSC_ADJUST
#endif

#ifdef MSR_HWCR
#undef MSR_HWCR
#endif

// MSR Addresses
static constexpr uint32_t MSR_TSC               = 0x10;
static constexpr uint32_t MSR_TSC_ADJUST        = 0x3B;
static constexpr uint32_t MSR_HWCR              = 0xC0010015;
static constexpr uint32_t MSR_CORE_THREAD_COUNT = 0x35;

// MSR Bits & Features
static constexpr uint64_t MSR_HWCR_LOCK_TSC_TO_CURR_P0 = getBit<uint64_t>(21);
static constexpr uint32_t CPUID_LEAF7_TSC_ADJUST       = getBit<uint32_t>(1);
static constexpr uint64_t CPUID_FEATURE_HTT            = getBit<uint64_t>(28);

// Sync interval in milliseconds (5 seconds)
static constexpr uint32_t PERIODIC_SYNC_INTERVAL = 5000;

// Power Management Trace Points
enum {
	kIOPMTracePointSleepCPUs = 0x18,
	kIOPMTracePointWakePlatformActions = 0x22,
};

// Singleton instance
static TSCForger instance {};
TSCForger &TSCForger::singleton() { return instance; }

// Core Logic

void TSCForger::lockFreq() {
	// AMD Family 17h+: Lock TSC to P0 frequency to prevent drift
	if (this->caps.amd17h) {
		wrmsr64(MSR_HWCR, rdmsr64(MSR_HWCR) | MSR_HWCR_LOCK_TSC_TO_CURR_P0);
	}
}

void TSCForger::sync(void *) {
	// 1. Maintain smoothness (ForgedInvariant feature)
	singleton().lockFreq();

	// 2. Determine the maximum TSC value across all cores
	__c11_atomic_fetch_max(&singleton().targetTSC, rdtsc64(), __ATOMIC_SEQ_CST);

	// 3. Barrier: Wait for all threads to reach this point
	atomic_fetch_add(&singleton().threadsEngaged, 1);
	while (atomic_load(&singleton().threadsEngaged) != singleton().threadCount) {
		// Spin lock
	}

	// 4. HARD SYNC: Write directly to MSR_TSC (CPUTscSync feature)
	// This fixes the black screen/wake issue on AMD by forcing exact alignment.
	wrmsr64(MSR_TSC, atomic_load_explicit(&singleton().targetTSC, memory_order_relaxed));
}

void TSCForger::syncAll(bool timer) {
	// Safety checks: Don't sync if sleeping or already syncing
	if (!atomic_load(&this->systemAwake) ||
		(!timer && atomic_load(&this->synchronised)) ||
		atomic_exchange(&this->synchronising, true)) {
		return;
	}

	atomic_store(&this->synchronised, false);

	// Reset counters for the rendezvous
	atomic_store(&this->threadsEngaged, 0);
	atomic_store(&this->targetTSC, 0);

	// Execute the sync on all cores immediately (blocking interrupts)
	mp_rendezvous_no_intrs(sync, nullptr);

	atomic_store(&this->synchronising, false);
	atomic_store(&this->synchronised, true);
}

// Hooks & Callbacks

void TSCForger::wrapXcpmUrgency(int urgency, uint64_t rtPeriod, uint64_t rtDeadline) {
	// Only proceed if TSC is currently synchronized
	if (!atomic_load_explicit(&singleton().synchronised, memory_order_relaxed)) { return; }
	FunctionCast(wrapXcpmUrgency, singleton().orgXcpmUrgency)(urgency, rtPeriod, rtDeadline);
}

void TSCForger::wrapTracePoint(void *that, uint8_t point) {
	switch (point) {
		case kIOPMTracePointSleepCPUs: {
			// System is going to sleep: Stop timer, mark as unsynced
			atomic_store_explicit(&singleton().systemAwake, false, memory_order_relaxed);
			atomic_store_explicit(&singleton().synchronised, false, memory_order_relaxed);
			singleton().stopTimer();
		} break;
		case kIOPMTracePointWakePlatformActions: {
			// System is waking up: Force sync immediately, restart timer
			atomic_store_explicit(&singleton().systemAwake, true, memory_order_relaxed);
			singleton().syncAll();
			singleton().startTimer();
		} break;
	}
	FunctionCast(wrapTracePoint, singleton().orgTracePoint)(that, point);
}

void TSCForger::wrapClockGetCalendarMicrotime(clock_sec_t *secs, clock_usec_t *microsecs) {
	// Ensure sync before getting time
	singleton().syncAll();
	FunctionCast(wrapClockGetCalendarMicrotime, singleton().orgClockGetCalendarMicrotime)(secs, microsecs);
}

// Initialization

void TSCForger::processPatcher(KernelPatcher &patcher) {
	this->syncAll();

	KernelPatcher::RouteRequest requests[] = {
		{"_xcpm_urgency", wrapXcpmUrgency, this->orgXcpmUrgency},
		{"__ZN14IOPMrootDomain10tracePointEh", wrapTracePoint, this->orgTracePoint},
		{"_clock_get_calendar_microtime", wrapClockGetCalendarMicrotime, this->orgClockGetCalendarMicrotime},
	};
	
	if (!patcher.routeMultiple(KernelPatcher::KernelID, requests)) {
		SYSLOG("TSCSyncer", "Failed to route symbols");
	}
}

void TSCForger::init() {
	uint32_t ebx, ecx, edx;

	SYSLOG("TSCSyncer", "Initializing Hybrid Version: Hard Sync + Freq Lock enabled.");

	const BaseDeviceInfo &info = BaseDeviceInfo::get();
	
	// 1. Detect Thread Count
	switch (info.cpuVendor) {
		case CPUInfo::CpuVendor::AMD: {
			// Use AMD specific CPUID to get core count
			if (CPUInfo::getCpuid(0x80000008, 0, nullptr, nullptr, &ecx)) {
				this->threadCount = (ecx & 0xFF) + 1;
			}
			
			// Detect AMD Family 17h+ for HWCR lock support
			union { CPUInfo::CpuVersion bits; uint32_t raw; } version;
			if (CPUInfo::getCpuid(1, 0, &version.raw)) {
				uint32_t family = version.bits.family;
				if (family == 0xF) family += version.bits.extendedFamily;
				this->caps.amd17h = (family >= 0x17);
			}
		} break;
		
		case CPUInfo::CpuVendor::Intel: {
			// Use MSR_CORE_THREAD_COUNT for Intel
			this->caps.tscAdjust = CPUInfo::getCpuid(7, 0, nullptr, &ebx) && (ebx & CPUID_LEAF7_TSC_ADJUST);
			if (info.cpuFamily > 6 || (info.cpuFamily == 6 && info.cpuModel > CPUInfo::CPU_MODEL_PENRYN)) {
				this->threadCount = rdmsr64(MSR_CORE_THREAD_COUNT) & 0xFFFF;
			}
		} break;
		
		default:
			SYSLOG("TSCSyncer", "Unknown CPU Vendor.");
			break;
	}

	// Fallback detection
	if (this->threadCount == 0) {
		if (CPUInfo::getCpuid(1, 0, nullptr, &ebx, &ecx, &edx)) {
			 uint64_t features = (static_cast<uint64_t>(ecx) << 32) | edx;
			 this->threadCount = (features & CPUID_FEATURE_HTT) ? ((ebx >> 16) & 0xFF) : 1;
		} else {
			this->threadCount = 1;
		}
	}

	DBGLOG("TSCSyncer", "Detected Thread Count: %d", this->threadCount);

	// 2. Register Patcher
	lilu.onPatcherLoadForce(
		[](void *user, KernelPatcher &patcher) { static_cast<TSCForger *>(user)->processPatcher(patcher); }, this);

	// 3. Start Timer (Periodic Sync)
	// Always enable this for maximum stability on Ryzentosh
	this->timer = IOTimerEventSource::timerEventSource(kOSBooleanTrue, timerAction);
	this->startTimer();
}

void TSCForger::startTimer() {
	if (this->timer) {
		this->timer->enable();
		this->timer->setTimeoutMS(PERIODIC_SYNC_INTERVAL);
	}
}

void TSCForger::stopTimer() {
	if (this->timer) {
		this->timer->cancelTimeout();
		this->timer->disable();
	}
}

void TSCForger::timerAction(OSObject *, IOTimerEventSource *sender) {
	singleton().syncAll(true);
	sender->setTimeoutMS(PERIODIC_SYNC_INTERVAL);
}

// Hope it work
// Great, work great
