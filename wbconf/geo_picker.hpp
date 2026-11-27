#ifndef WBCONF_GEO_PICKER_HPP
#define WBCONF_GEO_PICKER_HPP

/*
 * A place on Earth, picked on a world map, by city or by the time zone's
 * city (geo_picker.cpp). `get` says where it is now (nullopt: not set),
 * `set` takes a new place; the picker lives as long as its widget.
 */
#include <functional>
#include <optional>
#include <string>

#include <gtk/gtk.h>

#include "waybox/geo.hpp"

namespace wbconf {

GtkWidget *geo_picker_new(std::function<std::optional<wb::geo::Point>()> get,
		std::function<void(wb::geo::Point)> set);
/* The place changed elsewhere (its fields typed in): drawn again. */
void geo_picker_refresh(GtkWidget *picker);

/* For tests: the caption; a city found and taken (the first match); a
 * click at a fraction of the map's width and height. */
std::string geo_picker_caption(GtkWidget *picker);
void geo_picker_find(GtkWidget *picker, const std::string &query);
void geo_picker_click(GtkWidget *picker, double fx, double fy);
/* The crosshair clicked: the system asked (false: no portal to ask). */
bool geo_picker_locate(GtkWidget *picker);
/* The time zone's city taken, as its button does (false: no zone found). */
bool geo_picker_use_zone(GtkWidget *picker);

}  // namespace wbconf

#endif /* WBCONF_GEO_PICKER_HPP */
