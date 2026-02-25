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
    "-krnldbgdebug"
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
static char gTargetVolumeName[128] = {0};  // target volume name from krnldbglogvol=<name>
static char gTargetBSDName[64] = {0};      // target BSD device from krnldbglogbsd=<diskXsY>
static char gLogFileName[64] = "krnl.log"; // log file name (can be customized)
static char gTargetRelPath[160] = "/krnl.log"; // relative path under target volume
static bool gVolumeSearchActive = false;   // whether we're searching for a volume
static _Atomic(uint32_t) gVolumeSearchAttempts = 0; // number of search passes
static uint32_t gMaxVolumeSearchAttemptsBeforeFallback = 60; // after ~60 timer passes (~progressive seconds) fallback to /private/var/tmp
static void krnldbgTryInitFile();          // forward declaration
static void krnldbgSearchVolume();         // forward declaration
static bool krnldbgParseVolumePath(const char *path, char *outVol, size_t outVolSize, char *outRel, size_t outRelSize);

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
			if (gLogInitTimer) { gLogInitTimer->setTimeoutMS(gLogRetryMs); if (gLogRetryMs < 30000) gLogRetryMs <<= 1; }
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

	// Allow numbered variants: "name 1", "name 2", ...
	if (suffix[0] == ' ' && krnldbgIsDigit(suffix[1])) {
		suffix++;
		while (*suffix) {
			if (!krnldbgIsDigit(*suffix)) return false;
			suffix++;
		}
		return true;
	}

	// Allow APFS data volume suffix: "name - Data" and "name - Data 1"
	if (strncmp(suffix, " - Data", 7) == 0) {
		suffix += 7;
		if (suffix[0] == '\0') return true;
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

	if (ctx->targetBsd && ctx->targetBsd[0]) {
		const char *from = st->f_mntfromname;
		if (!from || !from[0]) return VFS_RETURNED;
		const char *fromBase = krnldbgBasename(from);
		const char *target = krnldbgNormalizeBsdName(ctx->targetBsd);
		if (strcmp(fromBase, target) != 0) return VFS_RETURNED;
	} else {
		if (!ctx->targetName || !ctx->targetName[0]) return VFS_RETURNED;
		const char *base = krnldbgBasename(mnt);
		if (!krnldbgMatchVolumeBase(base, ctx->targetName)) return VFS_RETURNED;
	}

	strlcpy(ctx->outPath, mnt, ctx->outPathSize);
	strlcat(ctx->outPath, ctx->relPath, ctx->outPathSize);
	ctx->found = true;
	return VFS_RETURNED_DONE;
}

// Search for target volume via mounted filesystem list
// Handles volume name variations like "test", "test 1", "test 2" and "test - Data"
static void krnldbgSearchVolume() {
	if (!gVolumeSearchActive || (gTargetVolumeName[0] == '\0' && gTargetBSDName[0] == '\0')) return;
	if (gLogReady) return; // already found

	uint32_t attempt = __c11_atomic_fetch_add(&gVolumeSearchAttempts, 1, __ATOMIC_RELAXED);
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
}

// Patcher load callback to obtain KernelPatcher instance
static void onPatcherLoad(void *user, KernelPatcher &patcher) {
	routeLogs(patcher);
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
	1,
	krnldbgdebug,
	1,
	krnldbgbeta,
	1,
	KernelVersion::Monterey,
	KernelVersion::Tahoe,
	[]() {
		lilu.onPatcherLoadForce(onPatcherLoad);
		Krnldbg.init();
	}
};