// The error screen when the game crashes, as the MHGU port's (its platform/android.c: fault_handler, the report,
// plat_fatal_notice, LogShareActivity): the screen that tells the player the game stopped and offers to share the log
// comes up instead of the app vanishing.
//   - a bad guest memory access: the runtime writes its report to the log and calls the FatalHook installed here
//     before it ends the process (mh4n/memory.h SetFatalHook);
//   - a crash of the host's own (SIGSEGV or SIGBUS outside the guest's memory, SIGABRT, SIGILL, SIGFPE, SIGTRAP): the
//     handlers here write the signal, the address, the thread and a backtrace to the log first.
// Either way the crashing thread only writes a byte to a pipe and waits (both async-signal-safe): a thread of the app's,
// attached to Java beforehand, opens the screen (GameActivity.crashReport: LogShareActivity in its own process, which
// outlives this one), and the process then ends. If that thread can't (Java itself stuck), the process ends after a few
// seconds anyway, as before.
// The game's own end with an error (Runtime::Boot failing) opens the same screen from android_main.cpp.
// A lost GPU device (the renderer's driver hook, gpu_driver.cpp) opens it too, as MHGU for Android's FATAL on a lost
// device does: before, the game froze on its last picture with 0 FPS until it was closed by hand (4u-native #47).
#pragma once

// android_main.cpp, before the game starts, on the thread SDL gave a JNIEnv (it finds GameActivity there).
void crash_screen_install();

// The GPU's device is lost (mh4n rhi DriverHooks::device_lost, on whichever thread saw it): the line in the log, the
// error screen, and the process ends. `where`: what said so (a submission, a fence, a wait) and after which submission.
void crash_screen_device_lost(const char* where);
