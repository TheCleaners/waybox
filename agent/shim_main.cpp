/*
 * waybox-mcp: the shim an agent starts as its MCP server (stdio), which
 * connects to the desktop's agent hub and passes MCP both ways
 * (doc/agent-tools.md, "Transport").
 *
 *   waybox-mcp [--token-file PATH] [--label NAME] [--lower KEY=LEVEL]...
 *
 * - The hub: $AGENT_TOOLS_SOCKET, else $XDG_RUNTIME_DIR/agents/tools/agent.
 * - The handshake first: one line each way. The token (read from its file,
 *   which must be only the user's) is sent only over this socket, never on
 *   a command line; --lower narrows levels for this connection;
 *   $AGENT_TOOLS_ASK is an ask ticket, if the hub started this agent to
 *   ask about a window.
 * - Then MCP, line by line, untouched. The agent done sending (stdin
 *   ends): the shim half-closes the socket and passes on what's still to
 *   come, until the hub closes.
 *
 * Reconnecting to a restarted hub comes with the call states.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "waybox/json.hpp"

namespace {

constexpr int kAnswerWaitMs = 10000;

[[noreturn]] void fail(const std::string &why) {
	fprintf(stderr, "waybox-mcp: %s\n", why.c_str());
	exit(1);
}

void usage(FILE *to) {
	fprintf(to, "usage: waybox-mcp [--token-file PATH] [--label NAME] [--lower KEY=LEVEL]...\n"
			"The desktop's tools for an assistant, as an MCP server on stdio.\n");
}

std::string socket_path() {
	if (const char *p = getenv("AGENT_TOOLS_SOCKET"); p != nullptr && *p != '\0')
		return p;
	const char *run = getenv("XDG_RUNTIME_DIR");
	if (run == nullptr || *run != '/')
		fail("XDG_RUNTIME_DIR isn't set: no session to reach");
	return std::string(run) + "/agents/tools/agent";
}

/* The token, from a file only the user can read. */
std::string read_token(const char *path) {
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		fail(std::string(path) + ": " + strerror(errno));
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
		fail(std::string(path) + ": not a file");
	if (st.st_uid != getuid() || (st.st_mode & 077) != 0)
		fail(std::string(path) + ": others can read it (it should be only yours: chmod 600)");
	std::string token;
	char buf[512];
	for (ssize_t n; (n = read(fd, buf, sizeof(buf))) != 0;) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fail(std::string(path) + ": " + strerror(errno));
		}
		token.append(buf, static_cast<size_t>(n));
		if (token.size() > 4096)
			fail(std::string(path) + ": too long for a token");
	}
	close(fd);
	while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' '))
		token.pop_back();
	if (token.empty())
		fail(std::string(path) + ": empty");
	return token;
}

bool write_all(int fd, const char *p, size_t n) {
	while (n > 0) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		p += w;
		n -= static_cast<size_t>(w);
	}
	return true;
}

int connect_hub(const std::string &path) {
	struct sockaddr_un addr {};
	addr.sun_family = AF_UNIX;
	if (path.size() >= sizeof(addr.sun_path))
		fail(path + ": too long a path");
	memcpy(addr.sun_path, path.c_str(), path.size() + 1);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		fail(std::string("socket: ") + strerror(errno));
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0)
		fail("the desktop's agent hub isn't running (" + path + ": " + strerror(errno) +
				"): agent tools may be off in the desktop's settings");
	return fd;
}

}  // namespace

int main(int argc, char **argv) {
	signal(SIGPIPE, SIG_IGN);
	wb::json::Object hs{{"agent-tools", 1.0}};
	wb::json::Object lower;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto value = [&]() -> const char * {
			if (i + 1 >= argc) {
				usage(stderr);
				exit(2);
			}
			return argv[++i];
		};
		if (a == "--token-file") {
			hs["token"] = read_token(value());
		} else if (a == "--label") {
			hs["label"] = std::string(value());
		} else if (a == "--lower") {
			std::string kv = value();
			size_t eq = kv.find('=');
			if (eq == std::string::npos || eq == 0 || eq + 1 == kv.size())
				fail("--lower " + kv + ": expected KEY=LEVEL");
			lower[kv.substr(0, eq)] = kv.substr(eq + 1);
		} else if (a == "--version") {
			printf("waybox-mcp %s\n", WB_SHIM_VERSION);
			return 0;
		} else if (a == "--help" || a == "-h") {
			usage(stdout);
			return 0;
		} else {
			usage(stderr);
			return 2;
		}
	}
	if (!lower.empty())
		hs["lower"] = lower;
	if (const char *ask = getenv("AGENT_TOOLS_ASK"); ask != nullptr && *ask != '\0')
		hs["ask"] = std::string(ask);
	unsetenv("AGENT_TOOLS_ASK");  /* (one use) */

	int sock = connect_hub(socket_path());
	std::string line = wb::json::dump(hs) + "\n";
	if (!write_all(sock, line.data(), line.size()))
		fail("the hub closed the connection");

	/* Its answer: one line (anything after it is MCP already). */
	std::string got;
	size_t nl;
	while ((nl = got.find('\n')) == std::string::npos) {
		struct pollfd p = {sock, POLLIN, 0};
		int r = poll(&p, 1, kAnswerWaitMs);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			fail("the hub didn't answer the handshake");
		char buf[4096];
		ssize_t n = read(sock, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			fail("the hub closed the connection during the handshake");
		got.append(buf, static_cast<size_t>(n));
		if (got.size() > 65536)
			fail("the hub's answer is too long");
	}
	auto answer = wb::json::parse(std::string_view(got).substr(0, nl));
	if (!answer || answer->get("ok") == nullptr || answer->get("ok")->boolean() == nullptr)
		fail("the hub's answer isn't one this shim understands");
	if (!*answer->get("ok")->boolean())
		fail("the hub refused: " + answer->str("error", "no reason given"));
	fprintf(stderr, "waybox-mcp: connected to %s, profile %s\n",
			answer->str("hub", "the hub").c_str(), answer->str("profile", "?").c_str());
	std::string rest = got.substr(nl + 1);
	if (!rest.empty() && !write_all(STDOUT_FILENO, rest.data(), rest.size()))
		return 0;

	/* MCP both ways, untouched. */
	bool stdin_open = true;
	char buf[65536];
	for (;;) {
		struct pollfd p[2] = {{sock, POLLIN, 0}, {stdin_open ? STDIN_FILENO : -1, POLLIN, 0}};
		if (poll(p, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			fail(std::string("poll: ") + strerror(errno));
		}
		if (p[1].revents != 0) {
			ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				/* The agent is done sending: the hub answers what's left. */
				stdin_open = false;
				shutdown(sock, SHUT_WR);
			} else if (!write_all(sock, buf, static_cast<size_t>(n))) {
				fail("the desktop's hub closed the connection");
			}
		}
		if (p[0].revents != 0) {
			ssize_t n = read(sock, buf, sizeof(buf));
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				if (stdin_open)
					fail("the desktop's hub closed the connection");
				return 0;
			}
			if (!write_all(STDOUT_FILENO, buf, static_cast<size_t>(n)))
				return 0;  /* the agent stopped reading: it's gone */
		}
	}
}
