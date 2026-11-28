#ifndef WB_CYCLE_OUTLINE_HPP
#define WB_CYCLE_OUTLINE_HPP


struct wb_server;
struct wb_toplevel;
struct wlr_scene_tree;

namespace wb {

/*
 * While the window switcher cycles, an outline around the whole frame of
 * the window that would be chosen (Openbox's focus cycle indicator): 6 px
 * inside the frame's edge, black with a white line along both sides, in
 * fixed colours (as Openbox's, so it shows on any theme). Above the
 * windows and panels, below the switcher's list. Gone with the switcher.
 */
class CycleOutline {
public:
	explicit CycleOutline(struct wb_server *server);
	~CycleOutline();
	CycleOutline(const CycleOutline &) = delete;
	CycleOutline &operator=(const CycleOutline &) = delete;

	/* Around `t` (nullptr: hidden). */
	void show(struct wb_toplevel *t);

	static constexpr int kWidth = 6;

private:
	struct wb_server *server_;
	struct wlr_scene_tree *tree_ = nullptr;
};

}  // namespace wb

#endif /* WB_CYCLE_OUTLINE_HPP */
