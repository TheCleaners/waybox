#ifndef WB_TEST_HPP
#define WB_TEST_HPP

/*
 * Minimal dependency-free unit-test harness.
 *
 * Each test file defines test cases with WB_TEST(name) { ... } and uses the
 * WB_CHECK* macros. Link the file against wb_test_main.cpp (see
 * test/meson.build); a non-zero process exit code means at least one check
 * failed, which is what `meson test` keys off of.
 *
 * Deliberately tiny: the compositor proper is covered by the headless ASan
 * smoke run, so these tests target only the pure, wlroots-free logic
 * (action registry/parsing, and future geometry/state helpers).
 */
#include <cstdio>
#include <functional>
#include <string_view>
#include <vector>

namespace wb::test {

struct Case {
	std::string_view name;
	std::function<void()> fn;
};

inline std::vector<Case> &registry() {
	static std::vector<Case> cases;
	return cases;
}

inline int &failure_count() {
	static int failures = 0;
	return failures;
}

struct Registrar {
	Registrar(std::string_view name, std::function<void()> fn) {
		registry().push_back({name, std::move(fn)});
	}
};

}  // namespace wb::test

#define WB_TEST(name)                                                        \
	static void wb_test_##name();                                            \
	static const ::wb::test::Registrar wb_test_reg_##name{#name,             \
			wb_test_##name};                                                 \
	static void wb_test_##name()

#define WB_CHECK(...)                                                        \
	do {                                                                     \
		if (!(__VA_ARGS__)) {                                                \
			++::wb::test::failure_count();                                   \
			std::fprintf(stderr, "  FAIL %s:%d: WB_CHECK(%s)\n", __FILE__,   \
					__LINE__, #__VA_ARGS__);                                 \
		}                                                                    \
	} while (0)

#define WB_CHECK_EQ(a, b)                                                    \
	do {                                                                     \
		if (!((a) == (b))) {                                                 \
			++::wb::test::failure_count();                                   \
			std::fprintf(stderr, "  FAIL %s:%d: WB_CHECK_EQ(%s, %s)\n",      \
					__FILE__, __LINE__, #a, #b);                             \
		}                                                                    \
	} while (0)

#endif /* WB_TEST_HPP */
