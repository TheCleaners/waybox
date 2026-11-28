#ifndef WB_CONFIRM_CARD_HPP
#define WB_CONFIRM_CARD_HPP

/*
 * A question with two answers, in the middle of the monitor under the
 * pointer, in the on-screen displays' style (the switcher's): a title, a
 * line under it, and two buttons, the safe one (Cancel) chosen first.
 * Exit's prompt (Openbox's <prompt>). It takes the keyboard and the
 * pointer while it's up (seat.cpp, cursor.cpp): Left, Right and Tab move
 * between the buttons, Return or Space press the chosen one, Escape
 * cancels; a click on a button presses it, one outside the card cancels.
 */
#include <cstdint>
#include <memory>
#include <string>

#include "waybox/geometry.hpp"
#include "waybox/scene_buffer.h"
#include "waybox/style.hpp"

struct wb_server;
struct wlr_scene_tree;

namespace wb {

class ConfirmCard {
public:
	ConfirmCard(struct wb_server *server, const SwitcherStyle &style, std::string title,
			std::string message, std::string yes, std::string no);
	~ConfirmCard();
	ConfirmCard(const ConfirmCard &) = delete;
	ConfirmCard &operator=(const ConfirmCard &) = delete;

	enum class Answer { None, Yes, No };
	/* A key (an xkb keysym): the answer, if it gave one. */
	Answer on_key(uint32_t sym);
	/* The pointer at layout (x, y): the button under it shown so. */
	void on_motion(double x, double y);
	/* A press at layout (x, y): a button's answer; outside the card, No. */
	Answer on_press(double x, double y);

private:
	void render();
	int button_at(double x, double y) const;  /* 0 No, 1 Yes, -1 none */

	struct wb_server *server_;
	SwitcherStyle style_;
	std::string title_, message_, yes_, no_;
	struct wlr_scene_tree *tree_ = nullptr;
	std::unique_ptr<SceneCanvas> canvas_;
	Rect rect_{0, 0, 0, 0};
	Rect buttons_[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};  /* card-local: No, Yes */
	int chosen_ = 0;   /* the button Return presses: No first */
	int hover_ = -1;
};

}  // namespace wb

#endif /* WB_CONFIRM_CARD_HPP */
