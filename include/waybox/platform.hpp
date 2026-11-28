#ifndef WB_PLATFORM_HPP
#define WB_PLATFORM_HPP

/*
 * Code that depends on the system names each system it supports, and has
 * a branch for any other that says so when it's compiled:
 *
 *   #if defined(__linux__)
 *   	...
 *   #elif defined(__FreeBSD__) || defined(__DragonFly__)
 *   	...
 *   #else
 *   	WB_PLATFORM_FALLBACK("the agent that started a shim isn't identified");
 *   	(what's left: the safe answer, "unknown")
 *   #endif
 *
 * Never "#else" meaning Linux: on a system that's neither, /proc code
 * compiles and quietly finds nothing. The fallback is shown on every build
 * that takes it, without failing it (warnings are errors here, and another
 * system should still build): a #pragma message, a note to GCC and a
 * warning Clang leaves out of -Werror. test/platform_branches_test.py
 * holds every system test to this.
 */

#define WB_PRAGMA_(x) _Pragma(#x)
#define WB_PLATFORM_FALLBACK(what) WB_PRAGMA_(message("not supported on this system: " what))

#endif /* WB_PLATFORM_HPP */
