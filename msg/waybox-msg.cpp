/*
 * waybox-msg: talk to Waybox's scripting socket (<waybox><ipc
 * enabled="yes"/>) from the shell. Prints the reply (one line of JSON;
 * pipe to jq to pick it apart), or each event as it comes.
 *
 *   waybox-msg get_windows | get_desktops | get_outputs | get_version
 *   waybox-msg action NAME [key=value ...]     e.g. action GoToDesktop to=2
 *                                              action Close window=app_id:foot
 *   waybox-msg identify [OUTPUT]               each monitor's name, on it
 *   waybox-msg pick_color                      the color clicked next
 *   waybox-msg subscribe window,desktop,output,config
 *   waybox-msg raw '{"request": ...}'
 *
 * The socket: $WAYBOX_SOCK, else $XDG_RUNTIME_DIR/waybox-$WAYLAND_DISPLAY.sock.
 */
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "waybox/json.hpp"
#include "version.h"

using wb::json::Array;
using wb::json::Object;
using wb::json::Value;

namespace {

int usage() {
	fprintf(stderr,
		"usage: waybox-msg get_windows|get_desktops|get_outputs|get_version\n"
		"       waybox-msg action NAME [key=value ...]   (to=, command=, window=, ...)\n"
		"       waybox-msg identify [OUTPUT]\n"
		"       waybox-msg pick_color\n"
		"       waybox-msg subscribe window,desktop,output,config\n"
		"       waybox-msg raw JSON\n");
	return 2;
}

std::string socket_path() {
	if (const char *s = getenv("WAYBOX_SOCK"); s && *s)
		return s;
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!runtime || !display)
		return {};
	return std::string(runtime) + "/waybox-" + display + ".sock";
}

/* "yes"/"true", "no"/"false" as booleans; the rest as strings. */
Value value_of(const std::string &v) {
	if (v == "true" || v == "yes")
		return true;
	if (v == "false" || v == "no")
		return false;
	return v;
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 2)
		return usage();
	std::string cmd = argv[1];
	if (cmd == "--version" || cmd == "-V") {
		printf("waybox-msg %s\n", WB_BUILD_VERSION);
		return 0;
	}
	Object req;
	bool stream = false;
	if (cmd == "raw") {
		if (argc != 3)
			return usage();
		auto v = wb::json::parse(argv[2]);
		if (!v || !v->object()) {
			fprintf(stderr, "waybox-msg: not a JSON object\n");
			return 2;
		}
		req = *v->object();
		stream = req.count("request") && req["request"].string() &&
				*req["request"].string() == "subscribe";
	} else if (cmd == "action") {
		if (argc < 3)
			return usage();
		req["request"] = "action";
		req["action"] = argv[2];
		for (int i = 3; i < argc; ++i) {
			std::string kv = argv[i];
			size_t eq = kv.find('=');
			if (eq == std::string::npos)
				return usage();
			req[kv.substr(0, eq)] = value_of(kv.substr(eq + 1));
		}
	} else if (cmd == "pick_color") {
		if (argc != 2)
			return usage();
		req["request"] = "pick_color";
	} else if (cmd == "identify") {
		if (argc > 3)
			return usage();
		req["request"] = "identify";
		if (argc == 3)
			req["output"] = argv[2];
	} else if (cmd == "subscribe") {
		if (argc != 3)
			return usage();
		Array events;
		std::string list = argv[2];
		size_t start = 0;
		while (start <= list.size()) {
			size_t comma = list.find(',', start);
			std::string e = list.substr(start, comma == std::string::npos
					? std::string::npos : comma - start);
			if (!e.empty())
				events.push_back(e);
			if (comma == std::string::npos)
				break;
			start = comma + 1;
		}
		req["request"] = "subscribe";
		req["events"] = events;
		stream = true;
	} else if (cmd.rfind("get_", 0) == 0) {
		if (argc != 2)
			return usage();
		req["request"] = cmd;
	} else {
		return usage();
	}

	std::string path = socket_path();
	struct sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (path.empty() || path.size() >= sizeof(addr.sun_path) || fd < 0) {
		fprintf(stderr, "waybox-msg: no socket (is <waybox><ipc enabled=\"yes\"/> set?)\n");
		return 1;
	}
	memcpy(addr.sun_path, path.c_str(), path.size() + 1);
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
		fprintf(stderr, "waybox-msg: %s: %s (is <waybox><ipc enabled=\"yes\"/> set?)\n",
				path.c_str(), strerror(errno));
		return 1;
	}
	std::string line = wb::json::dump(Value(req)) + "\n";
	if (write(fd, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
		fprintf(stderr, "waybox-msg: write: %s\n", strerror(errno));
		return 1;
	}
	std::string in;
	char buf[4096];
	bool first = true;
	int status = 0;
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		in.append(buf, static_cast<size_t>(n));
		size_t nl;
		while ((nl = in.find('\n')) != std::string::npos) {
			std::string reply = in.substr(0, nl);
			in.erase(0, nl + 1);
			auto v = wb::json::parse(reply);
			if (first) {
				first = false;
				const Value *ok = v ? v->get("ok") : nullptr;
				if (!ok || !ok->boolean() || !*ok->boolean()) {
					fprintf(stderr, "waybox-msg: %s\n",
							v ? v->str("error", reply).c_str() : reply.c_str());
					return 1;
				}
				if (stream)
					continue;  /* the acknowledgement; events follow */
			}
			printf("%s\n", reply.c_str());
			fflush(stdout);
			if (!stream)
				return status;
		}
	}
	close(fd);
	return first ? 1 : status;
}
