/*
 * The hub's audit log (audit.hpp).
 */
#include "audit.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace wb::agent {

namespace {

bool private_dir(const std::string &path, std::string &error) {
	/* Each part made only the user's (the state folder may not exist). */
	for (size_t i = 1; i <= path.size(); ++i) {
		if (i != path.size() && path[i] != '/')
			continue;
		std::string part = path.substr(0, i);
		if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) {
			error = part + ": " + strerror(errno);
			return false;
		}
	}
	struct stat st;
	if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
		error = path + ": not a folder of this user's";
		return false;
	}
	if ((st.st_mode & 077) != 0 && chmod(path.c_str(), 0700) != 0) {
		error = path + ": " + strerror(errno);
		return false;
	}
	return true;
}

bool write_all(int fd, const std::string &s) {
	size_t off = 0;
	while (off < s.size()) {
		ssize_t n = ::write(fd, s.data() + off, s.size() - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		off += static_cast<size_t>(n);
	}
	return true;
}

}  // namespace

Audit::Audit(std::string dir, const Tunables &t) : dir_(std::move(dir)), t_(t) {
	path_ = dir_ + "/audit.log";
	if (private_dir(dir_, error_))
		open_file();
}

Audit::~Audit() {
	if (fd_ >= 0)
		close(fd_);
}

void Audit::open_file() {
	if (fd_ >= 0)
		close(fd_);
	fd_ = open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd_ < 0) {
		error_ = path_ + ": " + strerror(errno);
		return;
	}
	struct stat st;
	if (fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
		error_ = path_ + ": not a file of this user's";
		close(fd_);
		fd_ = -1;
		return;
	}
	if ((st.st_mode & 077) != 0)
		fchmod(fd_, 0600);
	size_ = static_cast<size_t>(st.st_size);
}

/* audit.log → audit.log.1 → … (AuditKeep kept), and a new one. */
void Audit::rotate() {
	std::string oldest = path_ + "." + std::to_string(t_.audit_keep);
	unlink(oldest.c_str());
	for (size_t i = t_.audit_keep; i > 1; --i)
		rename((path_ + "." + std::to_string(i - 1)).c_str(), (path_ + "." + std::to_string(i)).c_str());
	rename(path_.c_str(), (path_ + ".1").c_str());
	open_file();
}

std::string Audit::now_iso() {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	gmtime_r(&ts.tv_sec, &tm);
	char buf[40];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tm.tm_year + 1900,
			tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
	return buf;
}

void Audit::write(json::Object line, bool important) {
	if (fd_ < 0)
		return;
	line["v"] = 1.0;
	line["time"] = now_iso();
	std::string text = json::dump(line) + "\n";
	if (size_ + text.size() > t_.audit_rotate && size_ > 0)
		rotate();
	if (fd_ < 0)
		return;
	/* One line, one write: lines never interleave. */
	if (!write_all(fd_, text))
		return;
	size_ += text.size();
	bool sync = t_.audit_sync == "all" || (t_.audit_sync == "important" && important);
	if (sync)
		fdatasync(fd_);
}

size_t Audit::recover() {
	std::ifstream f(path_);
	std::map<std::string, json::Object> open;  /* call id → its intent */
	std::vector<std::string> order;
	for (std::string line; std::getline(f, line);) {
		auto v = json::parse(line);
		if (!v || v->object() == nullptr)
			continue;
		std::string call = v->str("call");
		if (call.empty())
			continue;
		if (v->str("event") == "intent") {
			if (!open.count(call))
				order.push_back(call);
			open[call] = *v->object();
		} else if (v->str("event") == "call") {
			open.erase(call);
		}
	}
	size_t n = 0;
	for (const std::string &call : order) {
		auto it = open.find(call);
		if (it == open.end())
			continue;
		json::Object end = it->second;
		end.erase("v");
		end.erase("time");
		end.erase("args");
		end["event"] = "call";
		end["outcome"] = "outcome-unknown";
		end["why"] = "hub-restarted";
		write(std::move(end), true);
		++n;
	}
	return n;
}

json::Value Audit::audit_args(const json::Value &args, const std::set<std::string> &secrets) const {
	if (const json::Object *o = args.object()) {
		json::Object out;
		for (const auto &[k, v] : *o)
			out[k] = secrets.count(k) ? json::Value("<redacted>") : audit_args(v, secrets);
		return out;
	}
	if (const json::Array *a = args.array()) {
		json::Array out;
		for (const json::Value &v : *a)
			out.push_back(audit_args(v, secrets));
		return out;
	}
	if (const std::string *s = args.string(); s != nullptr && s->size() > t_.audit_cut) {
		/* (cut at a character's start, never inside one) */
		size_t at = t_.audit_cut;
		while (at > 0 && (static_cast<unsigned char>((*s)[at]) & 0xc0) == 0x80)
			--at;
		return json::Object{{"cut", true}, {"length", static_cast<double>(s->size())},
				{"start", s->substr(0, at)}};
	}
	return args;
}

}  // namespace wb::agent
