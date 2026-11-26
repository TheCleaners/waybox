#include "wb_test.hpp"

#include "waybox/name_list.hpp"

using wb::names::List;

WB_TEST(names_text) {
	WB_CHECK((wb::names::from_text("tray\n clock \n\n\ncustom/media") ==
			List{"tray", "clock", "custom/media"}));
	WB_CHECK(wb::names::from_text("").empty() && wb::names::from_text(" \n ").empty());
	WB_CHECK(wb::names::to_text({"a", "b"}) == "a\nb" && wb::names::to_text({}).empty());
	List l = {"x", "y z"};
	WB_CHECK(wb::names::from_text(wb::names::to_text(l)) == l);
}

WB_TEST(names_edit) {
	List l;
	WB_CHECK(wb::names::add(l, " tray ") && wb::names::add(l, "clock"));
	WB_CHECK(!wb::names::add(l, "tray") && !wb::names::add(l, "  ") &&
			!wb::names::add(l, "a\nb"));
	WB_CHECK((l == List{"tray", "clock"}));
	WB_CHECK(wb::names::move(l, 1, -1) && (l == List{"clock", "tray"}));
	WB_CHECK(!wb::names::move(l, 0, -1) && !wb::names::move(l, 1, 1) &&
			!wb::names::move(l, 5, 1) && !wb::names::move(l, 0, 0));
	wb::names::add(l, "cpu");
	WB_CHECK(wb::names::move(l, 0, 2) && (l == List{"tray", "cpu", "clock"}));
	WB_CHECK(wb::names::remove(l, 1) && (l == List{"tray", "clock"}));
	WB_CHECK(!wb::names::remove(l, 2));
}

WB_TEST(names_suggestions) {
	WB_CHECK((wb::names::not_in({"clock", "tray", "cpu", "tray"}, {"tray"}) ==
			List{"clock", "cpu"}));
	WB_CHECK(wb::names::not_in({}, {"a"}).empty());
}
