//
//  KernelDebugger.cpp
//  KernelDebugger
//
//  Created by hg13 on 30/05/1447 AH.
//

#include "KernelDebugger.h"
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>
#include <IOKit/IOTimerEventSource.h>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_file.hpp>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <IOKit/pwr_mgt/RootDomain.h>

// copyLoadedKextInfo will be used within the hotkey handler to get kext info for the dump.
typedef OSDictionary * (*copyLoadedKextInfoFunc)(OSArray * kextIdentifiers, OSArray * infoKeys);
static copyLoadedKextInfoFunc gcopyLoadedKextInfo = nullptr;

// Forward declarations for IOHIDSystem hook
class IOHIDSystem;

static int appendBufferToFile(const char *path, void *buffer, size_t size) {
	if (!buffer || size == 0) return 0;
	
	vnode_t vnode = NULLVP;
	vfs_context_t ctxt = vfs_context_create(nullptr);
	if (!ctxt) return ENOMEM;
	
	// Open file for writing, create if needed, do not truncate
	errno_t err = vnode_open(path, O_WRONLY | O_CREAT | O_NOFOLLOW, 
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH, VNODE_LOOKUP_NOFOLLOW, &vnode, ctxt);
	if (err) {
		vfs_context_rele(ctxt);
		return err;
	}
	
	// Get current file size to append at end
	off_t fileSize = 0;
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	if (vnode_getattr(vnode, &va, ctxt) == 0 && VATTR_IS_SUPPORTED(&va, va_data_size)) {
		fileSize = va.va_data_size;
	}
	
	// Write at end of file
	err = FileIO::writeFileData(buffer, fileSize, size, vnode, ctxt);
	
	// Close file
	errno_t closeErr = vnode_close(vnode, FWASWRITTEN, ctxt);
	vfs_context_rele(ctxt);
	
	return err ? err : closeErr;
}

static const char *krnldbgoff[] {
    "-krnldbgoff"
};

static const char *krnldbgdebug[] {
    "-krnldbgdebug",
    "-krnldbgkeylog"
};

static const char *krnldbgbeta[] {
    "-krnldbgbeta"
};

static krnldbg Krnldbg;
static void (*origConsoleWrite)(const char *, int) = nullptr;
static bool gInConsoleWriteHook = false;

// Line accumulation buffer — coalesces console_write fragments into complete lines
static const size_t kLineAccumMax = 4096;
static char gLineAccumBuf[kLineAccumMax];
static uint32_t gLineAccumPos = 0;
static clock_sec_t gLineAccumSecs = 0;
static clock_usec_t gLineAccumUsecs = 0;

// file logging controls
static bool gFileLoggingEnabled = true;            // disable with -krnldbglogdisable
static char gLogFilePath[256] = "/var/log/krnl.log"; // override with krnldbglogpath=<path>
static uint32_t gFlushIntervalMs = 1000;           // override with krnldbglogint=<ms>
static uint32_t gFlushThreshold = 8192;            // override with krnldbglogthresh=<bytes>
static _Atomic(uint32_t) gFlushedPos = 0;          // last flushed writePos (atomic to sync with writer)
static _Atomic(uint32_t) gOldestValidPos = 0;      // oldest data still valid in buffer (tracks overwrites)
static IOWorkLoop *gLogWorkLoop = nullptr;
static IOTimerEventSource *gLogTimer = nullptr;
static IOTimerEventSource *gLogInitTimer = nullptr; // retry timer for early file creation

// Simple ring buffer for captured log messages
static const size_t kLogBufMin = 64 * 1024;         // 64KB
static const size_t kLogBufMax = 250 * 1024 * 1024; // 250MB upper bound
static size_t gLogBufSize = 16 * 1024 * 1024;       // initial size 16MB, may grow via boot arg krnldbglogbuf=<KB> or -krnldbglogbig
static char *gLogBuffer = nullptr;
static _Atomic(uint32_t) gLogWritePos = 0; // monotonically increasing index (wrap via modulo when writing)
static _Atomic(uint64_t) gLogDropped = 0;  // count of dropped bytes/messages
static _Atomic(uint8_t) gInFlush = 0;      // atomic guard for flush logic (0/1)
static bool gLogReady = false;             // set when file successfully created/writable
static bool gInFileInit = false;           // recursion guard for init attempts
static uint32_t gLogRetryMs = 1500;        // exponential backoff for file creation
static uint32_t gLogInitUptimeSecMin = 5;  // do not attempt file IO before this uptime (configurable)
static uint32_t gFlushErrors = 0;          // consecutive flush write errors
static bool gForcedTmpPath = false;        // boot arg -krnldbglogtmp forces /private/var/tmp path
static bool gOpportunisticFlush = false;   // optional immediate flush on log writes
static _Atomic(bool) gSystemShuttingDown = false;  // set during shutdown/restart to prevent file ops
static IONotifier *gPowerNotifier = nullptr;       // power management notifier
static bool gSkipShutdownWait = false;             // cached from -krnldbgskipwait boot arg
static bool gSkipDataTruncation = false;           // cached from -krnldbglogfulldata boot arg
static void krnldbgFlushToFile(bool force);        // forward declaration

// Power event callback - detect shutdown/restart
static IOReturn krnldbgPowerEventHandler(void *target, void *refCon,
                                          UInt32 messageType, IOService *provider,
                                          void *messageArgument, vm_size_t argSize) {
	switch (messageType) {
		case kIOMessageSystemWillPowerOff:
			SYSLOG("krnldbg", "System powering off, disabling file operations");
			__c11_atomic_store(&gSystemShuttingDown, true, __ATOMIC_RELEASE);
			break;
		case kIOMessageSystemWillRestart:
			SYSLOG("krnldbg", "System restarting, disabling file operations");
			__c11_atomic_store(&gSystemShuttingDown, true, __ATOMIC_RELEASE);
			break;
		case kIOMessageSystemWillSleep:
			// Flush any pending data before sleep
			SYSLOG("krnldbg", "System going to sleep, flushing logs to file if needed");
			if (gLogReady && !__c11_atomic_load(&gSystemShuttingDown, __ATOMIC_ACQUIRE)) {
				krnldbgFlushToFile(true);
			}
			break;
	}
	return kIOReturnSuccess;
}
static char gTargetVolumeName[128] = {0};  // target volume name from krnldbglogvol=<name>
static char gTargetBSDName[64] = {0};      // target BSD device from krnldbglogbsd=<diskXsY>
static char gLogFileName[64] = "krnl.log"; // log file name (can be customized)
static char gTargetRelPath[160] = "/krnl.log"; // relative path under target volume
static bool gVolumeSearchActive = false;   // whether we're searching for a volume
static _Atomic(uint32_t) gVolumeSearchAttempts = 0; // number of search passes
static uint32_t gMaxVolumeSearchAttemptsBeforeFallback = 60; // after ~60 timer passes (~progressive seconds) fallback to /private/var/tmp
static bool gStalePlaceholderDetected = false;  // tracks if /Volumes/<target> is a stale directory (not a mount)
static void krnldbgTryInitFile();          // forward declaration
static void krnldbgSearchVolume();         // forward declaration
static bool krnldbgParseVolumePath(const char *path, char *outVol, size_t outVolSize, char *outRel, size_t outRelSize);

// Keyboard hotkey for IORegistry dump
static bool gHotkeyEnabled = true;                  // disable with -krnldbgnohotkey
static _Atomic(uint64_t) gLastDumpTime = 0;         // debounce timestamp (mach_absolute_time)
static _Atomic(uint8_t) gDumpInProgress = 0;        // prevent concurrent dumps
static char gIORegDumpPath[256] = {0};              // computed from gLogFilePath
static const uint64_t kDumpDebounceNs = 2000000000ULL; // 2 second debounce

// Kextstat dump (Ctrl+Shift+K)
static _Atomic(uint64_t) gLastKextDumpTime = 0;     // debounce for kext dump
static _Atomic(uint8_t) gKextDumpInProgress = 0;    // prevent concurrent kext dumps
static char gKextDumpPath[256] = {0};               // computed from gLogFilePath
static bool gHIDKeyLogEnabled = false;              // cached from boot arg at init

// Helper to compute IORegistry dump path from current log path
static void krnldbgUpdateIORegDumpPath() {
	if (!gHotkeyEnabled || !gLogFilePath[0]) return;
	const char *lastSlash = strrchr(gLogFilePath, '/');
	if (lastSlash) {
		size_t dirLen = lastSlash - gLogFilePath;
		memcpy(gIORegDumpPath, gLogFilePath, dirLen);
		strlcpy(gIORegDumpPath + dirLen, "/ioreg_dump.txt", sizeof(gIORegDumpPath) - dirLen);
		memcpy(gKextDumpPath, gLogFilePath, dirLen);
		strlcpy(gKextDumpPath + dirLen, "/kextstat_dump.txt", sizeof(gKextDumpPath) - dirLen);
	} else {
		strlcpy(gIORegDumpPath, "/private/var/log/ioreg_dump.txt", sizeof(gIORegDumpPath));
		strlcpy(gKextDumpPath, "/private/var/log/kextstat_dump.txt", sizeof(gKextDumpPath));
	}
}

// Forward declaration for IOHIDEventService
class IOHIDEventService;
static void (*origDispatchKeyboardEvent)(IOHIDEventService *, unsigned long long,
                                          unsigned, unsigned, unsigned, unsigned) = nullptr;
// Track modifier state (since HID events come separately)
static _Atomic(unsigned) gModifierState = 0;
static void krnldbgTriggerIORegDump();              // forward declaration
static void krnldbgScheduleIORegDump();             // forward declaration

// HID Usage codes (Keyboard page as 0x07) for hotkey detection
#define kHIDUsage_KeyboardD             0x07
#define kHIDUsage_KeyboardK             0x0E
#define kHIDUsage_KeyboardLeftControl   0xE0
#define kHIDUsage_KeyboardLeftShift     0xE1
#define kHIDUsage_KeyboardRightControl  0xE4
#define kHIDUsage_KeyboardRightShift    0xE5
#define kModCtrl  0x01
#define kModShift 0x02

// IOHIDFamily kext info for keyboard hook
static const char *kIOHIDFamilyPaths[] { "/System/Library/Extensions/IOHIDFamily.kext/Contents/MacOS/IOHIDFamily" };
static KernelPatcher::KextInfo kIOHIDFamilyInfo { "com.apple.iokit.IOHIDFamily", kIOHIDFamilyPaths, 1, {true}, {}, KernelPatcher::KextInfo::Unloaded };

// Flush one complete accumulated line into the ring buffer with its captured timestamp
static void krnldbgFlushAccumLine() {
	if (gLineAccumPos == 0 || !gLogBuffer) return;
	char line[kLineAccumMax + 64]; // room for timestamp prefix
	int prefixLen = snprintf(line, sizeof(line), "%llu.%06u ",
		(unsigned long long)gLineAccumSecs, (unsigned)gLineAccumUsecs);
	if (prefixLen <= 0 || prefixLen >= (int)sizeof(line)) { gLineAccumPos = 0; return; }
	int remaining = (int)sizeof(line) - prefixLen - 2; // room for \n + sentinel
	int copyLen = (int)gLineAccumPos < remaining ? (int)gLineAccumPos : remaining;
	memcpy(line + prefixLen, gLineAccumBuf, copyLen);
	int len = prefixLen + copyLen;
	if (line[len - 1] != '\n') { line[len] = '\n'; len++; }
	gLineAccumPos = 0;
	uint32_t total = (uint32_t)len + 1; // include sentinel
	uint32_t start = __c11_atomic_fetch_add(&gLogWritePos, total, __ATOMIC_RELAXED);
	uint32_t endPos = start + total;
	
	// Check if we're about to overwrite data that hasn't been flushed yet
	// We track gOldestValidPos to tell the flusher when data has been lost
	uint32_t oldestValid = __c11_atomic_load(&gOldestValidPos, __ATOMIC_RELAXED);
	
	// If the write would wrap and overwrite unflushed data, advance oldestValid
	if (endPos - oldestValid > gLogBufSize) {
		// Calculate new oldest valid position (just past what we're about to overwrite)
		uint32_t newOldest = endPos - (uint32_t)gLogBufSize;
		
		// Align to next line boundary for clean reading
		uint32_t scanLimit = start; // don't scan into area we're about to write
		uint32_t scanned = 0;
		const uint32_t maxScan = 2048;
		
		while (newOldest < scanLimit && scanned < maxScan) {
			uint32_t idx = newOldest % gLogBufSize;
			if (gLogBuffer[idx] == '\0') {
				newOldest++; // move past sentinel
				break;
			}
			newOldest++;
			scanned++;
		}
		
		// Atomically advance oldestValid (only if ours is newer)
		uint32_t expected = oldestValid;
		while (expected < newOldest) {
			if (__c11_atomic_compare_exchange_weak(&gOldestValidPos, &expected, newOldest, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
				__c11_atomic_fetch_add(&gLogDropped, newOldest - oldestValid, __ATOMIC_RELAXED);
				break;
			}
			// Another writer already advanced it further
			if (expected >= newOldest) break;
		}
	}
	
	// Write the log line to the buffer
	for (int i = 0; i < len; ++i) {
		uint32_t w = (start + i) % gLogBufSize;
		gLogBuffer[w] = line[i];
	}
	uint32_t sidx = (start + len) % gLogBufSize;
	gLogBuffer[sidx] = '\0';
	__c11_atomic_thread_fence(__ATOMIC_RELEASE); // publish completion
}

// Flush accumulated ring buffer data to file (append). Force flush ignores threshold.
static void krnldbgFlushToFile(bool force) {
	if (!gFileLoggingEnabled || !gLogReady || !gLogBuffer) return;
	if (!gSkipShutdownWait) {
		if (__c11_atomic_load(&gSystemShuttingDown, __ATOMIC_ACQUIRE)) return;
	}
	if (__c11_atomic_exchange(&gInFlush, 1, __ATOMIC_ACQUIRE)) return;
	
	// Snapshot positions atomically
	uint32_t writePos = __c11_atomic_load(&gLogWritePos, __ATOMIC_ACQUIRE);
	uint32_t flushedPos = __c11_atomic_load(&gFlushedPos, __ATOMIC_ACQUIRE);
	uint32_t oldestValid = __c11_atomic_load(&gOldestValidPos, __ATOMIC_ACQUIRE);
	__c11_atomic_thread_fence(__ATOMIC_ACQUIRE);
	
	// Check if there's anything to flush
	if (writePos == flushedPos) { __c11_atomic_store(&gInFlush, 0, __ATOMIC_RELEASE); return; }
	
	// Determine actual scan start - must be the later of flushedPos and oldestValid
	// If oldestValid > flushedPos, data was overwritten and we need to insert a gap marker
	uint32_t scanStart = flushedPos;
	bool needGapMarker = false;
	
	if (oldestValid > flushedPos) {
		// Data was overwritten! We need to start from oldestValid and note the gap
		needGapMarker = true;
		scanStart = oldestValid;
		// Update flushedPos to skip the lost data
		flushedPos = oldestValid;
		__c11_atomic_store(&gFlushedPos, flushedPos, __ATOMIC_RELEASE);
	}
	
	// Safety check: if scanStart is too far behind writePos, we have a problem
	uint32_t available = writePos - scanStart;
	if (available > gLogBufSize) {
		// This shouldn't happen if oldestValid tracking works, but handle it
		scanStart = writePos - (uint32_t)gLogBufSize;
		// Align to next complete line boundary
		uint32_t scanLimit = writePos;
		while (scanStart < scanLimit) {
			uint32_t idx = scanStart % gLogBufSize;
			if (gLogBuffer[idx] == '\0') { scanStart++; break; }
			scanStart++;
		}
		needGapMarker = true;
		__c11_atomic_fetch_add(&gLogDropped, available - (writePos - scanStart), __ATOMIC_RELAXED);
	}
	
	uint32_t scanEnd = writePos;
	uint32_t cursor = scanStart;
	uint32_t lastComplete = scanStart;
	uint32_t completeLines = 0;
	while (cursor < scanEnd) {
		uint32_t idx = cursor % gLogBufSize;
		if (gLogBuffer[idx] == '\0') { lastComplete = cursor + 1; completeLines++; }
		cursor++;
	}
	uint32_t completeSpan = lastComplete - scanStart;
	if (completeSpan == 0) { __c11_atomic_store(&gInFlush, 0, __ATOMIC_RELEASE); return; }
	if (!force && completeSpan < gFlushThreshold && completeSpan < (gLogBufSize / 2)) { __c11_atomic_store(&gInFlush, 0, __ATOMIC_RELEASE); return; }
	if (completeLines == 0) { __c11_atomic_store(&gInFlush, 0, __ATOMIC_RELEASE); return; }

	// Calculate gap marker size if needed
	const char *gapMarker = "*** LOG GAP: some entries were overwritten before flush ***\n";
	size_t gapMarkerLen = needGapMarker ? strlen(gapMarker) : 0;

	// First pass to compute total bytes required
	uint32_t pos = scanStart;
	uint32_t lineStart = scanStart;
	uint32_t totalBytes = (uint32_t)gapMarkerLen;
	while (pos < lastComplete) {
		uint32_t idx = pos % gLogBufSize;
		if (gLogBuffer[idx] == '\0') {
			uint32_t rawLen = pos - lineStart; // excludes sentinel
			totalBytes += rawLen;
			lineStart = pos + 1;
		}
		pos++;
	}
	if (totalBytes == 0) { __c11_atomic_store(&gInFlush, 0, __ATOMIC_RELEASE); return; }
	uint8_t *outBuf = (uint8_t *)IOMalloc(totalBytes);
	if (!outBuf) { __c11_atomic_fetch_add(&gLogDropped, completeSpan, __ATOMIC_RELAXED); __c11_atomic_store(&gInFlush, 0, __ATOMIC_RELEASE); return; }

	// Copy gap marker if needed
	uint32_t outPos = 0;
	if (needGapMarker) {
		memcpy(outBuf, gapMarker, gapMarkerLen);
		outPos = (uint32_t)gapMarkerLen;
	}

	// Second pass: copy lines in-order
	pos = scanStart;
	lineStart = scanStart;
	while (pos < lastComplete) {
		uint32_t idx = pos % gLogBufSize;
		if (gLogBuffer[idx] == '\0') {
			uint32_t rawLen = pos - lineStart;
			for (uint32_t i = 0; i < rawLen; ++i) {
				uint32_t ridx = (lineStart + i) % gLogBufSize;
				outBuf[outPos + i] = gLogBuffer[ridx];
			}
			outPos += rawLen;
			lineStart = pos + 1;
		}
		pos++;
	}
	int err = appendBufferToFile(gLogFilePath, outBuf, outPos);
	IOFree(outBuf, totalBytes);
	if (err == 0) {
		// Successfully flushed - update flushedPos
		__c11_atomic_store(&gFlushedPos, lastComplete, __ATOMIC_RELEASE);
		gFlushErrors = 0;
	} else {
		__c11_atomic_fetch_add(&gLogDropped, completeSpan, __ATOMIC_RELAXED);
		gFlushErrors++;
		if (gFlushErrors >= 3) {
			if ((err == 1 || err == 13 || err == 2) && !gForcedTmpPath && strcmp(gLogFilePath, "/private/var/tmp/krnl.log") != 0) {
				SYSLOG("krnldbg", "Flush err %d repeated (%u); switching to tmp path", err, gFlushErrors);
				strlcpy(gLogFilePath, "/private/var/tmp/krnl.log", sizeof(gLogFilePath));
				gForcedTmpPath = true;
				gLogReady = false;
				gFlushErrors = 0;
				if (gLogInitTimer) gLogInitTimer->setTimeoutMS(250); else krnldbgTryInitFile();
			}
		}
	}
	__c11_atomic_store(&gInFlush, 0, __ATOMIC_RELEASE);
}

static void krnldbgTryInitFile() {
	if (!gFileLoggingEnabled || gLogReady) return;
	if (!gSkipShutdownWait) {
		if (__c11_atomic_load(&gSystemShuttingDown, __ATOMIC_ACQUIRE)) return;
	}
	if (gInFileInit) return;
	gInFileInit = true;

	// If log path points under /Volumes but volume search isn't active, enable it to avoid
	// creating placeholder directories before the mount is ready.
	if (!gVolumeSearchActive) {
		SYSLOG("krnldbg", "Volume search not yet active, checking if log path '%s' indicates a volume to wait for...", gLogFilePath);
		char parsedVol[sizeof(gTargetVolumeName)] = {0};
		char parsedRel[sizeof(gTargetRelPath)] = {0};
		if (krnldbgParseVolumePath(gLogFilePath, parsedVol, sizeof(parsedVol), parsedRel, sizeof(parsedRel))) {
			strlcpy(gTargetVolumeName, parsedVol, sizeof(gTargetVolumeName));
			strlcpy(gTargetRelPath, parsedRel, sizeof(gTargetRelPath));
			gVolumeSearchActive = true;
		}
	}
	
	// If searching for a volume, try to find it first
	if (gVolumeSearchActive) {
		krnldbgSearchVolume();
		// If still searching (volume not found), just reschedule
		if (gVolumeSearchActive) {
			SYSLOG("krnldbg", "Volume '%s' not yet mounted, retrying...", gTargetVolumeName);
			// Use shorter interval during active volume search (2s), not exponential backoff
			if (gLogInitTimer) gLogInitTimer->setTimeoutMS(2000);
			gInFileInit = false;
			return;
		}
	}
	
	// Use nullptr with size 0 to just ensure file exists without writing data.
	int err = FileIO::writeBufferToFile(gLogFilePath, nullptr, 0, O_CREAT | FWRITE | O_NOFOLLOW, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if (err == 0) {
		gLogReady = true;
		// Flush backlog BEFORE emitting the readiness SYSLOG to keep earliest timestamps at file top.
		krnldbgFlushToFile(true);
		SYSLOG("krnldbg", "Log file ready: %s", gLogFilePath);
		if (gLogInitTimer) { gLogInitTimer->cancelTimeout(); gLogInitTimer->disable(); }
	} else {
		SYSLOG("krnldbg", "Log file not ready (err=%d) retry in %ums", err, gLogRetryMs);
		// Fallback after persistent EPERM/EACCES (1/13) to /private/var/tmp
        // maybe these errors mean nothing and the file still gets created after it was mounted?
		if ((err == 1 || err == 13) && strcmp(gLogFilePath, "/private/var/tmp/krnl.log") != 0) {
			static uint32_t attempts = 0; attempts++;
			if (attempts >= 3) {
				strlcpy(gLogFilePath, "/private/var/tmp/krnl.log", sizeof(gLogFilePath));
				SYSLOG("krnldbg", "Switching log path fallback to %s due to repeated permission errors", gLogFilePath);
				attempts = 0; // reset for future logic
			}
		}
		if (gLogInitTimer) { gLogInitTimer->setTimeoutMS(gLogRetryMs); if (gLogRetryMs < 30000) gLogRetryMs <<= 1; }
	}
	gInFileInit = false;
}

static bool krnldbgShouldAttemptInit() {
	clock_sec_t secs; clock_usec_t usecs; clock_get_system_microtime(&secs, &usecs);
	return secs >= gLogInitUptimeSecMin;
}

static const char *krnldbgBasename(const char *path) {
	if (!path || !path[0]) return path;
	const char *slash = strrchr(path, '/');
	return slash ? (slash + 1) : path;
}

static bool krnldbgIsDigit(char c) {
	return c >= '0' && c <= '9';
}

static const char *krnldbgNormalizeBsdName(const char *name) {
	if (!name) return name;
	const char *prefix = "/dev/";
	if (strncmp(name, prefix, 5) == 0) return name + 5;
	return name;
}

static bool krnldbgMatchVolumeBase(const char *base, const char *target) {
	if (!base || !target || !target[0]) return false;
	if (strcmp(base, target) == 0) return true;

	size_t targetLen = strlen(target);
	if (strncmp(base, target, targetLen) != 0) return false;
	const char *suffix = base + targetLen;
	if (suffix[0] == '\0') return true;

	// Accept numbered variants like "USB 1" when a stale placeholder exists
	// This handles the case where macOS renamed the volume due to a conflict
	if (gStalePlaceholderDetected && suffix[0] == ' ' && krnldbgIsDigit(suffix[1])) {
		const char *p = suffix + 1;
		while (*p) {
			if (!krnldbgIsDigit(*p)) break;
			p++;
		}
		if (*p == '\0') {
			SYSLOG("krnldbg", "Accepting numbered variant '%s' for target '%s' (stale placeholder exists)", base, target);
			return true;
		}
	}

	// Allow APFS data volume suffix: "name - Data"
	if (strncmp(suffix, " - Data", 7) == 0) {
		suffix += 7;
		if (suffix[0] == '\0') return true;
		// Also allow "name - Data 1" etc for APFS edge cases
		if (suffix[0] == ' ' && krnldbgIsDigit(suffix[1])) {
			suffix++;
			while (*suffix) {
				if (!krnldbgIsDigit(*suffix)) return false;
				suffix++;
			}
			return true;
		}
	}

	return false;
}

static bool krnldbgParseVolumePath(const char *path, char *outVol, size_t outVolSize, char *outRel, size_t outRelSize) {
	const char *prefix = "/Volumes/";
	if (!path || strncmp(path, prefix, strlen(prefix)) != 0) return false;
	const char *start = path + strlen(prefix);
	const char *slash = strchr(start, '/');
	if (!slash) {
		strlcpy(outVol, start, outVolSize);
		strlcpy(outRel, gTargetRelPath, outRelSize);
		return outVol[0] != '\0' && outRel[0] == '/';
	}
	if (slash == start) return false;

	size_t volLen = (size_t)(slash - start);
	if (volLen >= outVolSize) volLen = outVolSize - 1;
	memcpy(outVol, start, volLen);
	outVol[volLen] = '\0';

	if (slash[0] == '\0') {
		strlcpy(outRel, gTargetRelPath, outRelSize);
	} else {
		strlcpy(outRel, slash, outRelSize);
		if (outRel[0] != '/') {
			char tmp[160];
			snprintf(tmp, sizeof(tmp), "/%s", outRel);
			strlcpy(outRel, tmp, outRelSize);
		}
	}

	return outVol[0] != '\0' && outRel[0] == '/';
}

struct KdbgVolumeSearchCtx {
	const char *targetName;
	const char *targetBsd;
	const char *relPath;
	char *outPath;
	size_t outPathSize;
	bool found;
	vfs_context_t ctxt;
};

static int krnldbgVolumeIterate(mount_t mp, void *arg) {
	KdbgVolumeSearchCtx *ctx = reinterpret_cast<KdbgVolumeSearchCtx *>(arg);
	if (!ctx || !ctx->relPath) return VFS_RETURNED;
	if (ctx->ctxt) vfs_update_vfsstat(mp, ctx->ctxt, VFS_KERNEL_EVENT);
	struct vfsstatfs *st = vfs_statfs(mp);
	if (!st) return VFS_RETURNED;
	const char *mnt = st->f_mntonname;
	if (!mnt || !mnt[0]) return VFS_RETURNED;

	// Get mount flags and filesystem type for filtering
	uint64_t mntflags = vfs_flags(mp);
	char fstype[16] = {0};
	vfs_name(mp, fstype);

	// Log all mounts for debugging (only when volume search is active)
	const char *base = krnldbgBasename(mnt);
	DBGLOG("krnldbg", "VFS iter: '%s' (%s) flags=0x%llx blocks=%llu",
	       mnt, fstype, mntflags, (unsigned long long)st->f_blocks);

	// Must be read-write
	if (mntflags & MNT_RDONLY) {
		DBGLOG("krnldbg", "  -> skipped: read-only");
		return VFS_RETURNED;
	}

	// Reject autofs trigger mounts (transient placeholders before real mount)
	if (strcmp(fstype, "autofs") == 0) {
		DBGLOG("krnldbg", "  -> skipped: autofs");
		return VFS_RETURNED;
	}

	// Require actual backing storage (autofs/placeholders report zero blocks)
	if (st->f_blocks == 0 || st->f_bsize == 0) {
		DBGLOG("krnldbg", "  -> skipped: no storage");
		return VFS_RETURNED;
	}

	// Check if this matches our target
	if (ctx->targetBsd && ctx->targetBsd[0]) {
		const char *from = st->f_mntfromname;
		if (!from || !from[0]) return VFS_RETURNED;
		const char *fromBase = krnldbgBasename(from);
		const char *target = krnldbgNormalizeBsdName(ctx->targetBsd);
		if (strcmp(fromBase, target) != 0) {
			DBGLOG("krnldbg", "  -> skipped: BSD mismatch '%s' != '%s'", fromBase, target);
			return VFS_RETURNED;
		}
	} else {
		if (!ctx->targetName || !ctx->targetName[0]) return VFS_RETURNED;
		if (!krnldbgMatchVolumeBase(base, ctx->targetName)) {
			DBGLOG("krnldbg", "  -> skipped: name mismatch '%s' != '%s'", base, ctx->targetName);
			return VFS_RETURNED;
		}
	}

	SYSLOG("krnldbg", "Volume match found: %s", mnt);
	strlcpy(ctx->outPath, mnt, ctx->outPathSize);
	strlcat(ctx->outPath, ctx->relPath, ctx->outPathSize);
	ctx->found = true;
	return VFS_RETURNED_DONE;
}

// Search for target volume via mounted filesystem list
// Handles volume name variations like "test", "test 1", "test 2" and "test - Data"
// Check if a path exists and is NOT a mount point (just a directory on the parent filesystem)
// This detects stale placeholder directories in /Volumes
static bool krnldbgIsStaleVolumeDir(const char *volName) {
	if (!volName || !volName[0]) return false;

	char path[256];
	snprintf(path, sizeof(path), "/Volumes/%s", volName);

	vfs_context_t ctx = vfs_context_create(nullptr);
	if (!ctx) return false;

	vnode_t vp = NULLVP;
	errno_t err = vnode_lookup(path, 0, &vp, ctx);
	if (err || !vp) {
		vfs_context_rele(ctx);
		return false; // Path doesn't exist
	}

	// Check if this is a directory
	if (!vnode_isdir(vp)) {
		vnode_put(vp);
		vfs_context_rele(ctx);
		return false;
	}

	// Check if anything is mounted here by comparing mount points
	// If the directory's mount is the same as /Volumes, it's not a separate mount
	mount_t volMnt = vnode_mount(vp);
	vnode_put(vp);

	// Look up /Volumes to compare
	vnode_t volumesVp = NULLVP;
	err = vnode_lookup("/Volumes", 0, &volumesVp, ctx);
	if (err || !volumesVp) {
		vfs_context_rele(ctx);
		return false;
	}

	mount_t volumesMnt = vnode_mount(volumesVp);
	vnode_put(volumesVp);
	vfs_context_rele(ctx);

	// If they're on the same mount, /Volumes/USB is just a placeholder directory
	bool isStale = (volMnt == volumesMnt);
	if (isStale) {
		SYSLOG("krnldbg", "Detected stale placeholder directory: %s", path);
	}
	return isStale;
}

static void krnldbgSearchVolume() {
	if (!gVolumeSearchActive || (gTargetVolumeName[0] == '\0' && gTargetBSDName[0] == '\0')) return;
	if (gLogReady) return; // already found

	uint32_t attempt = __c11_atomic_fetch_add(&gVolumeSearchAttempts, 1, __ATOMIC_RELAXED);

	// On first few attempts, check for stale placeholder directories
	// that might cause macOS to rename the real volume (e.g., "USB" -> "USB 1")
	if (attempt <= 3 && gTargetVolumeName[0] && !gStalePlaceholderDetected) {
		gStalePlaceholderDetected = krnldbgIsStaleVolumeDir(gTargetVolumeName);
	}
	if ((attempt % 10) == 1) { // periodic progress
		if (gTargetBSDName[0]) {
			SYSLOG("krnldbg", "Mount search attempt %u for BSD '%s'", attempt, gTargetBSDName);
		} else {
			SYSLOG("krnldbg", "Volume search attempt %u for '%s'", attempt, gTargetVolumeName);
		}
	}
	
	KdbgVolumeSearchCtx ctx = {};
	ctx.targetName = gTargetVolumeName;
	ctx.targetBsd = gTargetBSDName;
	ctx.relPath = gTargetRelPath[0] ? gTargetRelPath : "/krnl.log";
	ctx.outPath = gLogFilePath;
	ctx.outPathSize = sizeof(gLogFilePath);
	ctx.found = false;
	ctx.ctxt = vfs_context_create(nullptr);
	if (vfs_iterate(0, krnldbgVolumeIterate, &ctx) == 0 && ctx.found) {
		SYSLOG("krnldbg", "Found target mount at %s", gLogFilePath);
		gVolumeSearchActive = false;
		// Update IORegistry dump path to match new log path location
		krnldbgUpdateIORegDumpPath();
		SYSLOG("krnldbg", "IORegistry dump path updated: %s", gIORegDumpPath);
		if (ctx.ctxt) vfs_context_rele(ctx.ctxt);
		return;
	}
	if (ctx.ctxt) vfs_context_rele(ctx.ctxt);

	// Fallback decision: if too many attempts, abandon volume search and use tmp path
	if (attempt >= gMaxVolumeSearchAttemptsBeforeFallback) {
		if (gTargetBSDName[0]) {
			SYSLOG("krnldbg", "BSD '%s' not found after %u attempts, falling back to /private/var/tmp", gTargetBSDName, attempt);
		} else {
			SYSLOG("krnldbg", "Volume '%s' not found after %u attempts, falling back to /private/var/tmp", gTargetVolumeName, attempt);
		}
		strlcpy(gLogFilePath, "/private/var/tmp/krnl.log", sizeof(gLogFilePath));
		gForcedTmpPath = true;
		gVolumeSearchActive = false; // stop searching
	}
	// Volume not found yet, will retry on next timer
}

static void krnldbgFlushTimer(OSObject *, IOTimerEventSource *) {
	// Force flush each interval to keep file roughly time-ordered and avoid large backlog.
	krnldbgFlushToFile(true);
	if (gLogTimer) gLogTimer->setTimeoutMS(gFlushIntervalMs);
}

// Accumulate console_write fragments and flush on newline (or buffer full).
// This coalesces many small writes into complete lines with a single timestamp.
static void krnldbgAccumConsoleWrite(const char *buf, int bufLen) {
	if (!buf || bufLen <= 0 || !gLogBuffer) return;
	for (int i = 0; i < bufLen; ++i) {
		char c = buf[i];
		// Capture timestamp at the start of each new line
		if (gLineAccumPos == 0)
			clock_get_system_microtime(&gLineAccumSecs, &gLineAccumUsecs);
		gLineAccumBuf[gLineAccumPos++] = c;
		// Flush on newline or when the accumulation buffer is full
		if (c == '\n' || gLineAccumPos >= kLineAccumMax - 1)
			krnldbgFlushAccumLine();
	}
}

static void hookedConsoleWrite(const char *buf, int len) {
	if (!origConsoleWrite) return;
	if (gInConsoleWriteHook) { origConsoleWrite(buf, len); return; }
	gInConsoleWriteHook = true;
	if (buf && len > 0) {
		krnldbgAccumConsoleWrite(buf, len);
		if (gOpportunisticFlush) krnldbgFlushToFile(false);
	}
	gInConsoleWriteHook = false;
	origConsoleWrite(buf, len);
}

// IORegistry dump: pretty-print helpers (ioreg -l style)

// Write tree prefix: "  |   |   " based on depth
static void krnldbgWriteTreePrefix(int depth, char *outBuf, size_t *outPos, size_t outMax, bool withBar = true) {
	for (int i = 0; i < depth && *outPos < outMax - 4; i++) {
		outBuf[(*outPos)++] = ' ';
		outBuf[(*outPos)++] = ' ';
		if (withBar && i < depth - 1) {
			outBuf[(*outPos)++] = '|';
			outBuf[(*outPos)++] = ' ';
		}
	}
}

// Forward declaration for recursive formatting
static void krnldbgFormatValue(OSObject *obj, char *outBuf, size_t *outPos, size_t outMax, int depth, bool compact);

// Format OSData as <hex bytes> with optional truncation
static void krnldbgFormatData(OSData *data, char *outBuf, size_t *outPos, size_t outMax) {
	if (!data || *outPos >= outMax - 16) return;

	unsigned int len = data->getLength();
	const uint8_t *bytes = (const uint8_t *)data->getBytesNoCopy();

	outBuf[(*outPos)++] = '<';

	// Limit hex output for very large data blobs unless truncation is disabled.
	unsigned int showLen = len;
	bool truncated = false;
	if (showLen > 64 && !gSkipDataTruncation) {
		showLen = 64;
		truncated = true;
	}

	for (unsigned int i = 0; i < showLen && *outPos < outMax - 8; i++) {
		if (i > 0 && (i % 4) == 0) outBuf[(*outPos)++] = ' ';
		int w = snprintf(outBuf + *outPos, outMax - *outPos, "%02x", bytes[i]);
		if (w > 0) *outPos += w;
	}

	if (truncated && *outPos < outMax - 32) {
		int w = snprintf(outBuf + *outPos, outMax - *outPos, "...%u total bytes", len);
		if (w > 0) *outPos += w;
	}

	if (*outPos < outMax) outBuf[(*outPos)++] = '>';
}

// Format OSDictionary as {"key"=value,...} (compact) or multi-line
static void krnldbgFormatDict(OSDictionary *dict, char *outBuf, size_t *outPos, size_t outMax, int depth, bool compact) {
	if (!dict || *outPos >= outMax - 8) return;

	outBuf[(*outPos)++] = '{';

	OSIterator *iter = OSCollectionIterator::withCollection(dict);
	if (iter) {
		bool first = true;
		OSSymbol *key;
		while ((key = OSDynamicCast(OSSymbol, iter->getNextObject()))) {
			OSObject *val = dict->getObject(key);
			if (!val) continue;

			if (!first) {
				outBuf[(*outPos)++] = ',';
			}
			first = false;

			// Write "key"=value
			int w = snprintf(outBuf + *outPos, outMax - *outPos, "\"%s\"=", key->getCStringNoCopy());
			if (w > 0) *outPos += w;

			krnldbgFormatValue(val, outBuf, outPos, outMax, depth + 1, true);

			if (*outPos >= outMax - 16) break;
		}
		iter->release();
	}

	if (*outPos < outMax) outBuf[(*outPos)++] = '}';
}

// Format OSArray as (item1,item2,...)
static void krnldbgFormatArray(OSArray *arr, char *outBuf, size_t *outPos, size_t outMax, int depth) {
	if (!arr || *outPos >= outMax - 8) return;

	outBuf[(*outPos)++] = '(';

	unsigned int count = arr->getCount();
	for (unsigned int i = 0; i < count && *outPos < outMax - 16; i++) {
		if (i > 0) outBuf[(*outPos)++] = ',';
		OSObject *obj = arr->getObject(i);
		if (obj) {
			krnldbgFormatValue(obj, outBuf, outPos, outMax, depth + 1, true);
		}
	}

	if (*outPos < outMax) outBuf[(*outPos)++] = ')';
}

// Format any OSObject value
static void krnldbgFormatValue(OSObject *obj, char *outBuf, size_t *outPos, size_t outMax, int depth, bool compact) {
	if (!obj || *outPos >= outMax - 32) return;

	// OSString
	if (OSString *str = OSDynamicCast(OSString, obj)) {
		const char *cstr = str->getCStringNoCopy();
		if (cstr) {
			int w = snprintf(outBuf + *outPos, outMax - *outPos, "\"%s\"", cstr);
			if (w > 0 && (size_t)w < outMax - *outPos) *outPos += w;
		}
		return;
	}

	// OSNumber
	if (OSNumber *num = OSDynamicCast(OSNumber, obj)) {
		unsigned long long val = num->unsigned64BitValue();
		unsigned int bits = num->numberOfBits();
		int w;
		// Show small numbers as decimal, large as hex
		if (val <= 9999999) {
			w = snprintf(outBuf + *outPos, outMax - *outPos, "%llu", val);
		} else if (bits <= 32) {
			w = snprintf(outBuf + *outPos, outMax - *outPos, "0x%llx", val);
		} else {
			w = snprintf(outBuf + *outPos, outMax - *outPos, "0x%llx", val);
		}
		if (w > 0) *outPos += w;
		return;
	}

	// OSBoolean
	if (OSBoolean *b = OSDynamicCast(OSBoolean, obj)) {
		const char *s = b->isTrue() ? "Yes" : "No";
		int w = snprintf(outBuf + *outPos, outMax - *outPos, "%s", s);
		if (w > 0) *outPos += w;
		return;
	}

	// OSData
	if (OSData *data = OSDynamicCast(OSData, obj)) {
		krnldbgFormatData(data, outBuf, outPos, outMax);
		return;
	}

	// OSDictionary (nested)
	if (OSDictionary *dict = OSDynamicCast(OSDictionary, obj)) {
		krnldbgFormatDict(dict, outBuf, outPos, outMax, depth, compact);
		return;
	}

	// OSArray
	if (OSArray *arr = OSDynamicCast(OSArray, obj)) {
		krnldbgFormatArray(arr, outBuf, outPos, outMax, depth);
		return;
	}

	// OSSymbol (treat like string)
	if (OSSymbol *sym = OSDynamicCast(OSSymbol, obj)) {
		const char *cstr = sym->getCStringNoCopy();
		if (cstr) {
			int w = snprintf(outBuf + *outPos, outMax - *outPos, "\"%s\"", cstr);
			if (w > 0 && (size_t)w < outMax - *outPos) *outPos += w;
		}
		return;
	}

	// Unknown type - show class name
	const OSMetaClass *mc = obj->getMetaClass();
	const char *cn = mc ? mc->getClassName() : "?";
	int w = snprintf(outBuf + *outPos, outMax - *outPos, "<%s>", cn);
	if (w > 0) *outPos += w;
}

// IORegistry dump: recursively dump registry tree (ioreg -l style)
static void krnldbgDumpIORegistryEntry(IORegistryEntry *entry,
                                        const IORegistryPlane *plane,
                                        int depth,
                                        char *outBuf,
                                        size_t *outPos,
                                        size_t outMax) {
	if (!entry || *outPos >= outMax - 2048) return;

	// Write tree prefix for entry line
	krnldbgWriteTreePrefix(depth, outBuf, outPos, outMax, false);

	// Get entry name and class
	const char *name = entry->getName(plane);
	if (!name) name = "(unnamed)";
	const char *className = entry->getMetaClass() ? entry->getMetaClass()->getClassName() : "?";

	// Get registry entry ID (like ioreg shows)
	uint64_t entryID = entry->getRegistryEntryID();

	// Write entry header: +-o Name  <class ClassName, id 0xXXX, retain N>
	int written = snprintf(outBuf + *outPos, outMax - *outPos,
	                       "+-o %s  <class %s, id 0x%llx, retain %d>\n",
	                       name, className, entryID, entry->getRetainCount());
	if (written > 0 && (size_t)written < outMax - *outPos) *outPos += written;

	// Get properties and format them
	OSDictionary *props = entry->dictionaryWithProperties();
	if (props && props->getCount() > 0) {
		// Opening brace
		krnldbgWriteTreePrefix(depth + 1, outBuf, outPos, outMax, true);
		int w = snprintf(outBuf + *outPos, outMax - *outPos, "| {\n");
		if (w > 0) *outPos += w;

		// Iterate properties
		OSIterator *iter = OSCollectionIterator::withCollection(props);
		if (iter) {
			OSSymbol *key;
			while ((key = OSDynamicCast(OSSymbol, iter->getNextObject()))) {
				OSObject *val = props->getObject(key);
				if (!val) continue;

				// Write prefix and key
				krnldbgWriteTreePrefix(depth + 1, outBuf, outPos, outMax, true);
				w = snprintf(outBuf + *outPos, outMax - *outPos, "|   \"%s\" = ", key->getCStringNoCopy());
				if (w > 0) *outPos += w;

				// Write value
				krnldbgFormatValue(val, outBuf, outPos, outMax, depth + 2, false);

				// Newline
				if (*outPos < outMax) outBuf[(*outPos)++] = '\n';

				if (*outPos >= outMax - 256) break;
			}
			iter->release();
		}

		// Closing brace
		krnldbgWriteTreePrefix(depth + 1, outBuf, outPos, outMax, true);
		w = snprintf(outBuf + *outPos, outMax - *outPos, "| }\n");
		if (w > 0) *outPos += w;

		props->release();
	} else if (props) {
		props->release();
	}

	// Recurse into children
	OSIterator *childIter = entry->getChildIterator(plane);
	if (childIter) {
		IORegistryEntry *child;
		while ((child = OSDynamicCast(IORegistryEntry, childIter->getNextObject()))) {
			krnldbgDumpIORegistryEntry(child, plane, depth + 1, outBuf, outPos, outMax);
		}
		childIter->release();
	}
}

static void krnldbgTriggerIORegDump() {
	// Don't attempt file operations during shutdown
	if (__c11_atomic_load(&gSystemShuttingDown, __ATOMIC_ACQUIRE)) {
		SYSLOG("krnldbg", "IORegistry dump skipped: system shutting down");
		return;
	}

	// Prevent concurrent dumps
	if (__c11_atomic_exchange(&gDumpInProgress, 1, __ATOMIC_ACQUIRE)) {
		SYSLOG("krnldbg", "IORegistry dump already in progress, skipping");
		return;
	}

	// Allocate large buffer (IORegistry can be 20-50MB without truncation)
	const size_t kDumpBufSize = 64 * 1024 * 1024;  // 64MB
	char *dumpBuf = (char *)IOMalloc(kDumpBufSize);
	if (!dumpBuf) {
		SYSLOG("krnldbg", "Failed to allocate IORegistry dump buffer");
		__c11_atomic_store(&gDumpInProgress, 0, __ATOMIC_RELEASE);
		return;
	}

	size_t dumpPos = 0;

	// Write header with timestamp
	clock_sec_t secs; clock_usec_t usecs;
	clock_get_system_microtime(&secs, &usecs);
	int hdr = snprintf(dumpBuf, kDumpBufSize,
	                   "IORegistry Dump at %llu.%06u\n"
	                   "======================================\n\n",
	                   (unsigned long long)secs, (unsigned)usecs);
	if (hdr > 0) dumpPos = hdr;

	// Get registry root and dump the tree
	IORegistryEntry *root = IORegistryEntry::getRegistryRoot();
	if (root) {
		krnldbgDumpIORegistryEntry(root, gIOServicePlane, 0, dumpBuf, &dumpPos, kDumpBufSize);
	} else {
		SYSLOG("krnldbg", "Failed to get IORegistry root");
	}

	// Write to file (but avoid /Volumes paths if volume not ready - would create phantom mounts)
	if (dumpPos > 0 && gIORegDumpPath[0]) {
		bool canWrite = true;
		if (strncmp(gIORegDumpPath, "/Volumes/", 9) == 0 && !gLogReady) {
			SYSLOG("krnldbg", "IORegistry dump skipped: volume not ready yet");
			canWrite = false;
		}
		if (canWrite) {
			int err = FileIO::writeBufferToFile(gIORegDumpPath, dumpBuf, dumpPos,
			                                     O_WRONLY | O_CREAT | O_TRUNC,
			                                     S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			if (err == 0) {
				SYSLOG("krnldbg", "IORegistry dumped to %s (%zu bytes)", gIORegDumpPath, dumpPos);
			} else {
				SYSLOG("krnldbg", "Failed to write IORegistry dump (err=%d)", err);
			}
		}
	}

	IOFree(dumpBuf, kDumpBufSize);
	__c11_atomic_store(&gDumpInProgress, 0, __ATOMIC_RELEASE);
}

// Dump loaded kexts (similar to kextstat | grep -v com.apple)
static void krnldbgTriggerKextDump() {
	// Don't attempt file operations during shutdown
	if (__c11_atomic_load(&gSystemShuttingDown, __ATOMIC_ACQUIRE)) {
		SYSLOG("krnldbg", "Kext dump skipped: system shutting down");
		return;
	}

	// Prevent concurrent dumps
	if (__c11_atomic_exchange(&gKextDumpInProgress, 1, __ATOMIC_ACQUIRE)) {
		SYSLOG("krnldbg", "Kext dump already in progress, skipping");
		return;
	}

	// Allocate buffer for output
	const size_t kBufSize = 256 * 1024;  // 256KB should be plenty
	char *dumpBuf = (char *)IOMalloc(kBufSize);
	if (!dumpBuf) {
		SYSLOG("krnldbg", "Failed to allocate kext dump buffer");
		__c11_atomic_store(&gKextDumpInProgress, 0, __ATOMIC_RELEASE);
		return;
	}

	size_t dumpPos = 0;

	// Write header
	clock_sec_t secs; clock_usec_t usecs;
	clock_get_system_microtime(&secs, &usecs);
	int hdr = snprintf(dumpBuf, kBufSize,
	                   "Loaded Kexts (non-Apple) at %llu.%06u\n"
	                   "==========================================\n"
	                   "Index  Refs  Address             Size        Name (Version)\n"
	                   "-----  ----  ------------------  ----------  --------------\n",
	                   (unsigned long long)secs, (unsigned)usecs);
	if (hdr > 0) dumpPos = hdr;

	// Get loaded kext info
	// gcopyLoadedKextInfo returns a dictionary keyed by bundle ID (resolved at runtime)
	OSDictionary *kextInfo = nullptr;
	if (gcopyLoadedKextInfo) {
		kextInfo = gcopyLoadedKextInfo(nullptr, nullptr);
	}

	if (kextInfo) {
		OSCollectionIterator *iter = OSCollectionIterator::withCollection(kextInfo);
		if (iter) {
			int index = 0;
			OSString *bundleID;
			while ((bundleID = OSDynamicCast(OSString, iter->getNextObject()))) {
				const char *idStr = bundleID->getCStringNoCopy();
				if (!idStr) continue;

				// Skip com.apple.* kexts
				if (strncmp(idStr, "com.apple.", 10) == 0) continue;

				OSDictionary *info = OSDynamicCast(OSDictionary, kextInfo->getObject(bundleID));
				if (!info) continue;

				// Get version
				const char *version = "?";
				OSString *verStr = OSDynamicCast(OSString, info->getObject("CFBundleVersion"));
				if (verStr) version = verStr->getCStringNoCopy();

				// Get load address
				uint64_t loadAddr = 0;
				OSNumber *addrNum = OSDynamicCast(OSNumber, info->getObject("OSBundleLoadAddress"));
				if (addrNum) loadAddr = addrNum->unsigned64BitValue();

				// Get size
				uint64_t loadSize = 0;
				OSNumber *sizeNum = OSDynamicCast(OSNumber, info->getObject("OSBundleLoadSize"));
				if (sizeNum) loadSize = sizeNum->unsigned64BitValue();

				// Get reference count
				uint32_t refs = 0;
				OSNumber *refNum = OSDynamicCast(OSNumber, info->getObject("OSBundleRetainCount"));
				if (refNum) refs = refNum->unsigned32BitValue();

				// Format line
				int written = snprintf(dumpBuf + dumpPos, kBufSize - dumpPos,
				                       "%5d  %4u  0x%016llx  %10llu  %s (%s)\n",
				                       index, refs, loadAddr, loadSize, idStr, version);
				if (written > 0 && dumpPos + written < kBufSize) {
					dumpPos += written;
				}
				index++;
			}
			iter->release();
		}
		kextInfo->release();
	} else {
		int written = snprintf(dumpBuf + dumpPos, kBufSize - dumpPos,
		                       "(Failed to get kext info)\n");
		if (written > 0) dumpPos += written;
	}

	// Write footer with count
	int footer = snprintf(dumpBuf + dumpPos, kBufSize - dumpPos,
	                      "\n==========================================\n");
	if (footer > 0 && dumpPos + footer < kBufSize) dumpPos += footer;

	// Write to file
	if (dumpPos > 0 && gKextDumpPath[0]) {
		bool canWrite = true;
		if (strncmp(gKextDumpPath, "/Volumes/", 9) == 0 && !gLogReady) {
			SYSLOG("krnldbg", "Kext dump skipped: volume not ready yet");
			canWrite = false;
		}
		if (canWrite) {
			int err = FileIO::writeBufferToFile(gKextDumpPath, dumpBuf, dumpPos,
			                                     O_WRONLY | O_CREAT | O_TRUNC,
			                                     S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			if (err == 0) {
				SYSLOG("krnldbg", "Kext list dumped to %s (%zu bytes)", gKextDumpPath, dumpPos);
			} else {
				SYSLOG("krnldbg", "Failed to write kext dump (err=%d)", err);
			}
		}
	}

	IOFree(dumpBuf, kBufSize);
	__c11_atomic_store(&gKextDumpInProgress, 0, __ATOMIC_RELEASE);
}

static void krnldbgScheduleKextDump() {
	// Schedule dump on workloop for thread safety
	if (gLogWorkLoop) {
		IOTimerEventSource *dumpTimer = IOTimerEventSource::timerEventSource(
			gLogWorkLoop,
			[](OSObject *, IOTimerEventSource *timer) {
				krnldbgTriggerKextDump();
				if (timer) {
					timer->cancelTimeout();
					gLogWorkLoop->removeEventSource(timer);
					timer->release();
				}
			}
		);
		if (dumpTimer && gLogWorkLoop->addEventSource(dumpTimer) == kIOReturnSuccess) {
			dumpTimer->setTimeoutMS(1);
			dumpTimer->enable();
			return;
		}
		if (dumpTimer) dumpTimer->release();
	}
	// Fallback: synchronous dump
	krnldbgTriggerKextDump();
}

static void krnldbgScheduleIORegDump() {
	// Schedule dump on workloop for thread safety (avoid interrupt context)
	if (gLogWorkLoop) {
		IOTimerEventSource *dumpTimer = IOTimerEventSource::timerEventSource(
			gLogWorkLoop,
			[](OSObject *, IOTimerEventSource *timer) {
				krnldbgTriggerIORegDump();
				if (timer) {
					timer->cancelTimeout();
					gLogWorkLoop->removeEventSource(timer);
					timer->release();
				}
			}
		);
		if (dumpTimer && gLogWorkLoop->addEventSource(dumpTimer) == kIOReturnSuccess) {
			dumpTimer->setTimeoutMS(1);  // Execute ASAP
			dumpTimer->enable();
			return;
		}
		if (dumpTimer) dumpTimer->release();
	}
	// Fallback: synchronous dump
	krnldbgTriggerIORegDump();
}

// Keyboard event hook for specified hotkeys (hooks IOHIDEventService::dispatchKeyboardEvent)
// Signature: dispatchKeyboardEvent(timestamp, usagePage, usage, value, options)
static void hookedDispatchKeyboardEvent(IOHIDEventService *self,
                                         unsigned long long timestamp,
                                         unsigned usagePage,
                                         unsigned usage,
                                         unsigned value,
                                         unsigned options) {
	// Only process keyboard page (0x07)
	if (usagePage == 0x07) {
		// Debug: log key events if keylog was enabled at boot (cached, not checked per-event)
		if (gHIDKeyLogEnabled) {
			DBGLOG("krnldbg", "HID Key: usage=0x%x value=%u options=0x%x", usage, value, options);
		}

		// Track modifier state
		if (usage == kHIDUsage_KeyboardLeftControl || usage == kHIDUsage_KeyboardRightControl) {
			if (value) __c11_atomic_fetch_or(&gModifierState, kModCtrl, __ATOMIC_RELAXED);
			else __c11_atomic_fetch_and(&gModifierState, ~kModCtrl, __ATOMIC_RELAXED);
		}
		if (usage == kHIDUsage_KeyboardLeftShift || usage == kHIDUsage_KeyboardRightShift) {
			if (value) __c11_atomic_fetch_or(&gModifierState, kModShift, __ATOMIC_RELAXED);
			else __c11_atomic_fetch_and(&gModifierState, ~kModShift, __ATOMIC_RELAXED);
		}

		// Check for Ctrl+Shift+D hotkey (D key pressed)
		if (gHotkeyEnabled && value == 1 && usage == kHIDUsage_KeyboardD) {
			unsigned mods = __c11_atomic_load(&gModifierState, __ATOMIC_RELAXED);
			if ((mods & (kModCtrl | kModShift)) == (kModCtrl | kModShift)) {
				// Debounce check (2 seconds between dumps)
				uint64_t now = mach_absolute_time();
				uint64_t last = __c11_atomic_load(&gLastDumpTime, __ATOMIC_RELAXED);

				if (now - last > kDumpDebounceNs) {
					if (__c11_atomic_compare_exchange_strong(&gLastDumpTime, &last, now,
					                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
						SYSLOG("krnldbg", "Hotkey Ctrl+Shift+D detected, triggering IORegistry dump");
						krnldbgScheduleIORegDump();
					}
				}
			}
		}

		// Check for Ctrl+Shift+K hotkey (K key pressed) - kextstat dump
		if (gHotkeyEnabled && value == 1 && usage == kHIDUsage_KeyboardK) {
			unsigned mods = __c11_atomic_load(&gModifierState, __ATOMIC_RELAXED);
			if ((mods & (kModCtrl | kModShift)) == (kModCtrl | kModShift)) {
				uint64_t now = mach_absolute_time();
				uint64_t last = __c11_atomic_load(&gLastKextDumpTime, __ATOMIC_RELAXED);

				if (now - last > kDumpDebounceNs) {
					if (__c11_atomic_compare_exchange_strong(&gLastKextDumpTime, &last, now,
					                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
						SYSLOG("krnldbg", "Hotkey Ctrl+Shift+K detected, triggering kext list dump");
						krnldbgScheduleKextDump();
					}
				}
			}
		}
	}
	if (origDispatchKeyboardEvent) {
		origDispatchKeyboardEvent(self, timestamp, usagePage, usage, value, options);
	}
}

static void routeLogs(KernelPatcher &patcher) {
	// Hook the required logging function
	KernelPatcher::RouteRequest reqs[] {
		KernelPatcher::RouteRequest("_console_write", hookedConsoleWrite, reinterpret_cast<mach_vm_address_t &>(origConsoleWrite))
	};

	if (!patcher.routeMultiple(KernelPatcher::KernelID, reqs, arrsize(reqs))) {
		SYSLOG("krnldbg", "Failed to route some log symbols");
	} else {
		SYSLOG("krnldbg", "Hooked all necessary log symbols");
	}

	// Resolve copyLoadedKextInfo for kextstat dump
	mach_vm_address_t kextInfoAddr = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN6OSKext18copyLoadedKextInfoEP7OSArrayS1_");
	if (!kextInfoAddr) {
		// Fallback to older symbol name (first symbol should be present on most cases)
		kextInfoAddr = patcher.solveSymbol(KernelPatcher::KernelID, "_OSKextCopyLoadedKextInfo");
	}
	if (kextInfoAddr) {
		gcopyLoadedKextInfo = reinterpret_cast<copyLoadedKextInfoFunc>(kextInfoAddr);
		SYSLOG("krnldbg", "Resolved copyLoadedKextInfo for kextstat dump");
	} else {
		SYSLOG("krnldbg", "Failed to resolve copyLoadedKextInfo, Ctrl+Shift+K disabled");
	}
}

// Kext load callback for IOHIDFamily - hooks keyboard events
static void onIOHIDFamilyLoad(void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (!gHotkeyEnabled) return;

	// Guard against double-hooking (callback may fire multiple times)
	if (origDispatchKeyboardEvent) return;

	SYSLOG("krnldbg", "IOHIDFamily detected, installing keyboard hook");

	KernelPatcher::RouteRequest kbdReqs[] {
		KernelPatcher::RouteRequest("__ZN17IOHIDEventService21dispatchKeyboardEventEyjjjj",
		                            hookedDispatchKeyboardEvent,
		                            reinterpret_cast<mach_vm_address_t &>(origDispatchKeyboardEvent))
	};

	if (!patcher.routeMultiple(index, kbdReqs, arrsize(kbdReqs))) {
		SYSLOG("krnldbg", "Failed to hook keyboard event in IOHIDFamily, hotkeys disabled");
		gHotkeyEnabled = false;
	} else {
		SYSLOG("krnldbg", "Keyboard hook installed successfully in IOHIDFamily");
	}
}

// Patcher load callback to obtain KernelPatcher instance
static void onPatcherLoad(void *user, KernelPatcher &patcher) {
	routeLogs(patcher);

	// Keyboard hotkey: check boot arg to disable
	if (checkKernelArgument("-krnldbgnohotkey")) {
		gHotkeyEnabled = false;
		kIOHIDFamilyInfo.switchOff();
		SYSLOG("krnldbg", "Hotkey detection disabled by boot arg");
	}

	// Cache HID key logging setting (expensive to check per-event)
	gHIDKeyLogEnabled = checkKernelArgument("-krnldbgkeylog");
	if (gHIDKeyLogEnabled) {
		SYSLOG("krnldbg", "HID key logging enabled (may leak sensitive info, use with caution)");
	}

	// Cache shutdown wait skip setting (checked frequently in flush path)
	gSkipShutdownWait = checkKernelArgument("-krnldbgskipwait");

	// Skip truncating OSData in IORegistry logs (show full data, may cause very large log entries)
	gSkipDataTruncation = checkKernelArgument("-krnldbglogfulldata");

	if (checkKernelArgument("-krnldbglogdisable")) gFileLoggingEnabled = false;
	if (checkKernelArgument("-krnldbglogtmp")) {
		strlcpy(gLogFilePath, "/private/var/tmp/krnl.log", sizeof(gLogFilePath));
		gForcedTmpPath = true;
	}
	// Allow global filename override (used by volume and root-path modes)
	PE_parse_boot_argn("krnldbglogname", gLogFileName, sizeof(gLogFileName));
	// Volume-based logging: krnldbglogvol=VolumeName (searches /Volumes/ for the volume)
	if (PE_parse_boot_argn("krnldbglogvol", gTargetVolumeName, sizeof(gTargetVolumeName))) {
		gVolumeSearchActive = true;
		snprintf(gTargetRelPath, sizeof(gTargetRelPath), "/%s", gLogFileName);
		SYSLOG("krnldbg", "Volume logging enabled: searching for volume '%s', file '%s'", gTargetVolumeName, gLogFileName);
		gTargetBSDName[0] = '\0';
	}
	// BSD-based logging: krnldbglogbsd=diskXsY (resolves mount by device)
	if (PE_parse_boot_argn("krnldbglogbsd", gTargetBSDName, sizeof(gTargetBSDName))) {
		gVolumeSearchActive = true;
		snprintf(gTargetRelPath, sizeof(gTargetRelPath), "/%s", gLogFileName);
		SYSLOG("krnldbg", "BSD logging enabled: searching for device '%s', file '%s'", gTargetBSDName, gLogFileName);
	}
	// Early init: start file creation immediately (uptime = 0)
	if (checkKernelArgument("-krnldbgloginitfast")) {
		gLogInitUptimeSecMin = 0;
		SYSLOG("krnldbg", "Early file init enabled (uptime min = 0)");
	}
	// Enable opportunistic flush on every log write (default off)
	if (checkKernelArgument("-krnldbglogopport")) {
		gOpportunisticFlush = true;
		SYSLOG("krnldbg", "Opportunistic flush enabled");
	}
	// Buffer size in MB (overrides -krnldbglogbig)
	uint32_t bufMB = 0;
	if (PE_parse_boot_argn("krnldbglogbufmb", &bufMB, sizeof(bufMB))) {
		if (bufMB >= 1 && bufMB <= 250) {
			gLogBufSize = (size_t)bufMB * 1024ULL * 1024ULL;
			SYSLOG("krnldbg", "Buffer size set to %uMB", bufMB);
		}
	} else if (checkKernelArgument("-krnldbglogbig")) {
		// Large buffer (64MB) if not explicitly overridden later
		gLogBufSize = 64 * 1024 * 1024;
	}
	if (gFileLoggingEnabled) {
		bool logPathProvided = false;
		if (PE_parse_boot_argn("krnldbglogpath", gLogFilePath, sizeof(gLogFilePath))) {
			logPathProvided = true;
			char parsedVol[sizeof(gTargetVolumeName)] = {0};
			char parsedRel[sizeof(gTargetRelPath)] = {0};
			if (krnldbgParseVolumePath(gLogFilePath, parsedVol, sizeof(parsedVol), parsedRel, sizeof(parsedRel))) {
				strlcpy(gTargetVolumeName, parsedVol, sizeof(gTargetVolumeName));
				strlcpy(gTargetRelPath, parsedRel, sizeof(gTargetRelPath));
				gVolumeSearchActive = true;
				SYSLOG("krnldbg", "Volume logging via path: searching for '%s', rel '%s'", gTargetVolumeName, gTargetRelPath);
				gTargetBSDName[0] = '\0';
			} else {
				gVolumeSearchActive = false;
				gTargetVolumeName[0] = '\0';
				gTargetBSDName[0] = '\0';
			}
		}
		// Shorter root path options to avoid long NVRAM strings
		if (!logPathProvided) {
			char relPath[160] = {0};
			char dirPath[160] = {0};
			if (PE_parse_boot_argn("krnldbglogrel", relPath, sizeof(relPath))) {
				logPathProvided = true;
				if (relPath[0] == '\0') {
					strlcpy(gLogFilePath, "/private/var/tmp/krnl.log", sizeof(gLogFilePath));
				} else if (relPath[0] == '/') {
					strlcpy(gLogFilePath, relPath, sizeof(gLogFilePath));
				} else {
					snprintf(gLogFilePath, sizeof(gLogFilePath), "/%s", relPath);
				}
				// If rel is a directory, append filename
				size_t len = strlen(gLogFilePath);
				if (len > 0 && gLogFilePath[len - 1] == '/') {
					strlcat(gLogFilePath, gLogFileName, sizeof(gLogFilePath));
				}
			} else if (PE_parse_boot_argn("krnldbglogdir", dirPath, sizeof(dirPath))) {
				logPathProvided = true;
				if (dirPath[0] == '/') {
					strlcpy(gLogFilePath, dirPath, sizeof(gLogFilePath));
				} else {
					snprintf(gLogFilePath, sizeof(gLogFilePath), "/%s", dirPath);
				}
				size_t len = strlen(gLogFilePath);
				if (len == 0 || gLogFilePath[len - 1] != '/') {
					strlcat(gLogFilePath, "/", sizeof(gLogFilePath));
				}
				strlcat(gLogFilePath, gLogFileName, sizeof(gLogFilePath));
			}
			if (logPathProvided) {
				char parsedVol[sizeof(gTargetVolumeName)] = {0};
				char parsedRel[sizeof(gTargetRelPath)] = {0};
				if (krnldbgParseVolumePath(gLogFilePath, parsedVol, sizeof(parsedVol), parsedRel, sizeof(parsedRel))) {
					strlcpy(gTargetVolumeName, parsedVol, sizeof(gTargetVolumeName));
					strlcpy(gTargetRelPath, parsedRel, sizeof(gTargetRelPath));
					gVolumeSearchActive = true;
					SYSLOG("krnldbg", "Volume logging via short path: searching for '%s', rel '%s'", gTargetVolumeName, gTargetRelPath);
					gTargetBSDName[0] = '\0';
				}
			}
		}
		PE_parse_boot_argn("krnldbglogint", &gFlushIntervalMs, sizeof(gFlushIntervalMs));
		PE_parse_boot_argn("krnldbglogthresh", &gFlushThreshold, sizeof(gFlushThreshold));
		uint32_t bufKB = 0;
		if (PE_parse_boot_argn("krnldbglogbuf", &bufKB, sizeof(bufKB))) {
			// Clamp to allowed range 64KB..256000KB (250MB)
			if (bufKB >= 64) {
				if (bufKB > 256000) bufKB = 256000;
				gLogBufSize = (size_t)bufKB * 1024ULL;
			}
		}
	}
	if (!gForcedTmpPath && !strncmp(gLogFilePath, "/var/", 5)) {
		char remap[sizeof(gLogFilePath)];
		snprintf(remap, sizeof(remap), "/private%s", gLogFilePath);
		strlcpy(gLogFilePath, remap, sizeof(gLogFilePath));
		SYSLOG("krnldbg", "Remapped log path to %s", gLogFilePath);
	}
	// Compute IORegistry dump path from log path (same directory, ioreg_dump.txt)
	if (gHotkeyEnabled && gLogFilePath[0]) {
		krnldbgUpdateIORegDumpPath();
		SYSLOG("krnldbg", "IORegistry dump path: %s", gIORegDumpPath);
	}
	// Allocate log buffer now (after potential resize)
	// Validate final size bounds
	if (gLogBufSize < kLogBufMin) gLogBufSize = kLogBufMin;
	if (gLogBufSize > kLogBufMax) gLogBufSize = kLogBufMax;
	gLogBuffer = (char *)IOMalloc(gLogBufSize);
	if (!gLogBuffer) {
		SYSLOG("krnldbg", "Failed to allocate log buffer size %zu, disabling file logging", gLogBufSize);
		gFileLoggingEnabled = false;
	}
	if (gFileLoggingEnabled) {
		if (gVolumeSearchActive && !gLogReady && (gTargetVolumeName[0] || gTargetBSDName[0])) {
			char pending[256];
			if (gTargetBSDName[0]) {
				snprintf(pending, sizeof(pending), "bsd:%s%s", gTargetBSDName, gTargetRelPath);
			} else {
				snprintf(pending, sizeof(pending), "/Volumes/%s%s", gTargetVolumeName, gTargetRelPath);
			}
			SYSLOG("krnldbg", "Logging active pending=%s interval=%ums threshold=%u buf=%zuKB", pending, gFlushIntervalMs, gFlushThreshold, gLogBufSize/1024);
		} else {
			SYSLOG("krnldbg", "Logging active path=%s interval=%ums threshold=%u buf=%zuKB", gLogFilePath, gFlushIntervalMs, gFlushThreshold, gLogBufSize/1024);
		}
		gLogWorkLoop = IOWorkLoop::workLoop();
		if (gLogWorkLoop) {
			gLogWorkLoop->retain();
			gLogTimer = IOTimerEventSource::timerEventSource(gLogWorkLoop, krnldbgFlushTimer);
			if (gLogTimer && gLogWorkLoop->addEventSource(gLogTimer) == kIOReturnSuccess) {
				gLogTimer->setTimeoutMS(gFlushIntervalMs);
				gLogTimer->enable();
				SYSLOG("krnldbg", "Flush timer started");
			} else {
				SYSLOG("krnldbg", "Failed to start flush timer");
				if (gLogTimer) gLogTimer->release();
				gLogTimer = nullptr;
			}
			gLogInitTimer = IOTimerEventSource::timerEventSource(gLogWorkLoop, [](OSObject *, IOTimerEventSource *) {
				if (krnldbgShouldAttemptInit()) {
					krnldbgTryInitFile();
				} else {
					// Reschedule quickly until minimum uptime reached
					if (gLogInitTimer) gLogInitTimer->setTimeoutMS(500);
				}
			});
			if (gLogInitTimer && gLogWorkLoop->addEventSource(gLogInitTimer) == kIOReturnSuccess) {
				gLogInitTimer->setTimeoutMS(500);
				gLogInitTimer->enable();
				SYSLOG("krnldbg", "Init timer started");
				// First attempt only after uptime condition met (checked by timer lambda)
			} else {
				SYSLOG("krnldbg", "Failed to start init timer");
				if (gLogInitTimer) gLogInitTimer->release();
				gLogInitTimer = nullptr;
			}
			gLogWorkLoop->release();
		}

		// Register for power management events (shutdown/restart detection)
		// IOPMrootDomain may not be available during early boot, so use a timer to retry
		IOTimerEventSource *powerRegTimer = IOTimerEventSource::timerEventSource(gLogWorkLoop,
			[](OSObject *, IOTimerEventSource *timer) {
				if (gPowerNotifier) {
					// Already registered
					if (timer) {
						timer->cancelTimeout();
						gLogWorkLoop->removeEventSource(timer);
						timer->release();
					}
					return;
				}

				// Find IOPMrootDomain by class name
				OSDictionary *matching = IOService::serviceMatching("IOPMrootDomain");
				if (matching) {
					IOService *rootDomain = IOService::copyMatchingService(matching);
					matching->release();
					if (rootDomain) {
						gPowerNotifier = rootDomain->registerInterest(gIOPriorityPowerStateInterest,
						                                               krnldbgPowerEventHandler, nullptr, nullptr);
						rootDomain->release();
						if (gPowerNotifier) {
							SYSLOG("krnldbg", "Registered for power events");
							if (timer) {
								timer->cancelTimeout();
								gLogWorkLoop->removeEventSource(timer);
								timer->release();
							}
							return;
						}
					}
				}
				// Retry in 2 seconds
				if (timer) timer->setTimeoutMS(2000);
			}
		);
		if (powerRegTimer && gLogWorkLoop->addEventSource(powerRegTimer) == kIOReturnSuccess) {
			powerRegTimer->setTimeoutMS(1000);  // First attempt after 1 second
			powerRegTimer->enable();
		} else {
			if (powerRegTimer) powerRegTimer->release();
		}
	}
}

void krnldbg::init() {
    SYSLOG("krnldbg", "We debugging the kernel with this one");
    if (checkKernelArgument("-krnldbgdebug")) {
        DBGLOG("krnldbg", "Never really thought you would debug a kext that debugs the kernel that's being debugged");
    }
}

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	krnldbgoff,
	arrsize(krnldbgoff),
	krnldbgdebug,
	arrsize(krnldbgdebug),
	krnldbgbeta,
	arrsize(krnldbgbeta),
	KernelVersion::Monterey,
	KernelVersion::Tahoe,
	[]() {
		lilu.onPatcherLoadForce(onPatcherLoad);
		lilu.onKextLoadForce(&kIOHIDFamilyInfo, 1, onIOHIDFamilyLoad);
		Krnldbg.init();
	}
};