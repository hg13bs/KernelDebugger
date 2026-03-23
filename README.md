# KernelDebugger

Finally, a kext that can dump kernel debug messages to a file for later analysis without the need of a serial connection or screen capture. Also useful on recoveryOS or the installer stage.

## Requirements

- An x86_64 (Intel/AMD) based Hackintosh or Mac.
- OpenCore bootloader (recommended, duh).
- macOS Monterey or newer (possibly lower too, but untested).

## Building from Source

1. Clone the repository: `git clone https://github.com/hg13bs/KernelDebugger`
2. Initalize submodules: `git submodule update --init --recursive`
3. Open `KernelDebugger.xcodeproj` in Xcode or build using `xcodebuild` from the command line (e.g. `xcodebuild -scheme KernelDebugger -configuration Release`).

## Installation

1. Download the latest release from the [Releases](https://github.com/hg13bs/KernelDebugger/releases) page.
2. Ensure your config.plist is configured to have the kext loaded properly
3. Optionally, add desired boot arguments to control the kext's behavior (see below).

> [!NOTE]
> Don't forget to remove/disable this kext while not debugging as you may leave out a bunch of I/O consumption or memory usage to happen in the background.
> Additionally, be cautious when enabling keyboard input logging as it may capture sensitive information.

> [!TIP]
> Some kernel/kext logs may not show without adding [DebugEnhancer](https://github.com/acidanthera/DebugEnhancer) to your kexts.

### Boot Arguments

This kext comes with a number of boot arguments to control its behavior. Add them to OpenCore's `boot-args` variable as needed.

#### General

- `-krnldbgoff`: Disable the kext entirely.
- `-krnldbgbeta`: Force loading on unsupported macOS versions (use with caution).
- `-krnldbgdebug`: Enable debug logging for the kext itself.
- `-krnldbgkeylog`: Log HID keyboard input (may include sensitive data, use with caution).
- `-krnldbgskipwait`: Ignores shutdown checks in the log flushing function, allowing it to run even during shutdown.
- `krnldbgnohotkey`: Disable hotkey support for triggering various diagnotic actions.
- `-krnldbglogfulldata`: Disable truncation of large OSData blobs in IORegistry logs, showing the full content instead (may cause very large log entries).

#### Keyboard hotkeys

- Ctrl+Shift+D: Dumps the current IORegistry to a file named `ioreg_dump.txt` in the same directory as the log file.
- Ctrl+Shift+K: Dumps loaded non-Apple kext information to a file named `kextstat_dump.txt` in the same directory as the log file.

#### File Logging

- `-krnldbglogdisable`: Disable file logging (ring buffer still captures messages).
- `-krnldbglogtmp`: Force log output to `/private/var/tmp/krnl.log`.
- `-krnldbgloginitfast`: Begin file creation immediately at boot (skip the default 5-second uptime wait).
- `-krnldbglogopport`: Flush to disk on every log write instead of waiting for the timer (higher I/O, lower latency).
- `-krnldbglogbig`: Use a large 16 MB ring buffer (same as `krnldbglogbufmb=16`).

#### Log Path & Destination

- `krnldbglogpath` (`string`, default: `/var/log/krnl.log`): Full path for the log file.
- `krnldbglogname` (`string`, default: `krnl.log`): Override just the log filename (used with volume/dir modes).
- `krnldbglogrel` (`string`, default: none): Short relative/absolute path (avoids long NVRAM strings).
- `krnldbglogdir` (`string`, default: none): Directory path; the log filename is appended automatically.
- `krnldbglogvol` (`string`, default: none): Volume name to search for under `/Volumes/` (e.g. `USB` finds `/Volumes/USB`).
- `krnldbglogbsd` (`string`, default: none): BSD device name to resolve mount point (e.g. `disk2s1`).

> **Volume resolution:** When using `krnldbglogvol` or `krnldbglogbsd`, the kext iterates mounted filesystems until the target appears, with automatic fallback to `/private/var/tmp/krnl.log` after ~60 retries. APFS data-volume variants (e.g. `Name - Data`) and numbered duplicates (`Name 1`) are matched automatically.

#### Buffer & Flush Tuning

- `krnldbglogbuf` (`uint32` KB, default: `16384`): Ring buffer size in KB (range: 64–256000).
- `krnldbglogbufmb` (`uint32` MB, default: `16`): Ring buffer size in MB (range: 1–250, overrides `krnldbglogbuf`).
- `krnldbglogint` (`uint32` ms, default: `1000`): Flush timer interval in milliseconds.
- `krnldbglogthresh` (`uint32` bytes, default: `8192`): Minimum buffered bytes before a non-forced flush writes to disk.

## Issues & Limitations

- Not all log sources may be captured, especially early boot messages before the kext initializes and including some user-space logs that don't go through the hooked functions.

- File I/O is performed in a workqueue context, which may be deferred during heavy system load or panic conditions.

- The kext may not have enough time to dump the kernel messages into a file before the kernel reboots while the target partition may not have fully finished mounting.

- For others, see the [Issues](https://github.com/hg13bs/KernelDebugger/issues) page.

## Credits

- [Apple](https://www.apple.com) for macOS (duh)
- [Acidanthera](https://github.com/acidanthera) for Lilu/MacKernelSDK
- [Anthropic](https://www.anthropic.com) for Claude, which was used to help with the kext implementation and code documentation.

## License

This project is licensed under MIT, see the [LICENSE](LICENSE) file for details.
