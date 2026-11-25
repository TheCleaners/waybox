/*
 * A stand-in for a session program (wbconf_move_test): takes any
 * arguments and waits until it's stopped. A binary, not a script, so it
 * runs under the name it's copied to everywhere (on FreeBSD a script's
 * process is named after its interpreter).
 */
#include <unistd.h>

int main(void) {
	for (;;)
		pause();
}
