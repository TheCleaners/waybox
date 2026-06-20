#include "wb_test.hpp"

/* Shared entry point for the unit-test executables: run every registered case
 * and report. Exit status is the number of failed checks (clamped), so a
 * non-zero code fails `meson test`. */
int main() {
	for (const ::wb::test::Case &test_case : ::wb::test::registry()) {
		std::printf("[ RUN  ] %.*s\n", static_cast<int>(test_case.name.size()),
				test_case.name.data());
		test_case.fn();
	}

	int failures = ::wb::test::failure_count();
	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("all checks passed\n");
	return 0;
}
