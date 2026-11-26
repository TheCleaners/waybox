#include "wbconf_ui.hpp"

namespace wbconf {

/* ---- Fonts + live theme preview -------------------------------------- */

/* The installed font families, sorted and de-duplicated. Uses the default
 * Cairo/Pango font map, so no widget is needed. */
const std::vector<std::string> &font_families() {
	static const std::vector<std::string> families = [] {
		std::vector<std::string> out;
		PangoFontMap *fm = pango_cairo_font_map_get_default();
		PangoFontFamily **fams = nullptr;
		int n = 0;
		pango_font_map_list_families(fm, &fams, &n);
		out.reserve(n);
		for (int i = 0; i < n; i++) {
			const char *name = pango_font_family_get_name(fams[i]);
			if (name != nullptr)
				out.emplace_back(name);
		}
		g_free(fams);
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}();
	return families;
}

}  // namespace wbconf
