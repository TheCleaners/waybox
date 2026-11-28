/*
 * Every call an agent makes (calls.hpp).
 */
#include "calls.hpp"

#include <glib.h>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <set>

#include "providers.hpp"

namespace wb::agent {

namespace {

enum class State { Waiting, Dispatched, CancellationRequested, Ended };

struct Call {
	std::string id;
	Caller who;
	OfferedTool tool;
	json::Object args;
	CallSink sink;          /* cleared once the agent's told, or gone */
	State state = State::Waiting;
	std::chrono::steady_clock::time_point received, dispatched;
	unsigned handle = 0;
	guint timeout = 0, deadline = 0;
	bool high_impact = false;
	bool holds_slot = false;  /* from dispatch until the provider answers or goes */
	~Call() {
		if (timeout != 0)
			g_source_remove(timeout);
		if (deadline != 0)
			g_source_remove(deadline);
	}
};

struct Calls {
	Tunables t;
	Audit *audit = nullptr;
	bool locked = false;
	std::map<std::string, std::unique_ptr<Call>> calls;
	std::deque<std::string> waiting;
	std::map<unsigned, size_t> by_agent;
	std::map<std::string, size_t> by_provider;
	std::map<unsigned, std::deque<std::chrono::steady_clock::time_point>> recent;  /* CallRate */
	unsigned next = 1;
};

Calls *g = nullptr;

bool high_impact(const OfferedTool &t) {
	const std::string &s = t.manifest.sensitivity;
	return s == "exec" || s == "destroy" || s == "capture" || s == "input" ||
			t.manifest.reach == "outside";
}

json::Object line_for(const Call &c, const char *event) {
	std::set<std::string> secrets(c.tool.manifest.secret.begin(), c.tool.manifest.secret.end());
	json::Object agent{{"conn", static_cast<double>(c.who.conn)}, {"profile", c.who.profile}};
	if (!c.who.label.empty())
		agent["label"] = c.who.label;
	if (!c.who.claim.empty())
		agent["claim"] = c.who.claim;
	return json::Object{
		{"event", event}, {"call", c.id}, {"agent", std::move(agent)},
		{"provider", c.tool.provider}, {"tool", c.tool.tool}, {"name", c.tool.name},
		{"sensitivity", c.tool.manifest.sensitivity},
		{"args", g->audit != nullptr ? g->audit->audit_args(c.args, secrets) : json::Value()},
	};
}

double ms_since(std::chrono::steady_clock::time_point t) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void release(Call &c);
void dispatch_waiting();

/* The call's end, in the audit; the agent told (if it's still waiting to
 * be). */
void end(Call &c, const char *outcome, const char *why, json::Object extra = {}) {
	if (c.state == State::Ended)
		return;
	c.state = State::Ended;
	/* (its slot stays taken until the provider answers: see release) */
	if (g->audit != nullptr) {
		json::Object line = line_for(c, "call");
		line["decision"] = std::string(outcome) == "refused" ? "refused" : "allowed";
		line["outcome"] = outcome;
		line["why"] = why;
		line["ms"] = ms_since(c.received);
		for (auto &[k, v] : extra)
			line[k] = std::move(v);
		g->audit->write(std::move(line), c.high_impact);
	}
}

void erase(const std::string &id);

/* A slot, freed only when the provider is done with the call (it answered,
 * or went away): never at outcome unknown, so a provider that ignores
 * cancellation can't be sent more than MaxInFlight. */
void release(Call &c) {
	if (!c.holds_slot)
		return;
	c.holds_slot = false;
	if (g->by_agent[c.who.conn] > 0)
		--g->by_agent[c.who.conn];
	if (g->by_provider[c.tool.provider] > 0)
		--g->by_provider[c.tool.provider];
	/* (a slot is free: the next waiting call may go) */
	g_idle_add([](gpointer) -> gboolean {
		dispatch_waiting();
		return G_SOURCE_REMOVE;
	}, nullptr);
}

void erase(const std::string &id) {
	if (g == nullptr)
		return;
	auto it = g->calls.find(id);
	if (it == g->calls.end())
		return;
	release(*it->second);
	g->calls.erase(it);
}

/* Dispatched and no answer will come in time: outcome unknown. The
 * provider is asked to stop; an answer later is recorded, not sent. */
void unknown(Call &c, const char *why) {
	if (c.state != State::Dispatched && c.state != State::CancellationRequested)
		return;
	if (c.sink.complete)
		c.sink.complete(json::Object{
			{"content", json::Array{json::Object{{"type", "text"}, {"text", outcome_unknown_text(why)}}}},
			{"isError", true},
		});
	c.sink = {};
	providers_cancel(c.handle);
	end(c, "outcome-unknown", why);
	/* (kept for a late answer: dropped when it comes, or with the
	 * provider) */
}

gboolean on_timeout(gpointer data) {
	auto *id = static_cast<std::string *>(data);
	auto it = g != nullptr ? g->calls.find(*id) : decltype(g->calls)::iterator{};
	if (g != nullptr && it != g->calls.end()) {
		it->second->timeout = 0;
		unknown(*it->second, "CallTimeout");
	}
	return G_SOURCE_REMOVE;
}

gboolean on_deadline(gpointer data) {
	auto *id = static_cast<std::string *>(data);
	auto it = g != nullptr ? g->calls.find(*id) : decltype(g->calls)::iterator{};
	if (g != nullptr && it != g->calls.end()) {
		it->second->deadline = 0;
		unknown(*it->second, "CallDeadline");
	}
	return G_SOURCE_REMOVE;
}

void arm_timeout(Call &c) {
	if (c.timeout != 0)
		g_source_remove(c.timeout);
	c.timeout = g_timeout_add_full(G_PRIORITY_DEFAULT, static_cast<guint>(g->t.call_timeout.count()),
			on_timeout, new std::string(c.id), [](gpointer d) { delete static_cast<std::string *>(d); });
}

void dispatch(Call &c) {
	c.state = State::Dispatched;
	c.dispatched = std::chrono::steady_clock::now();
	++g->by_agent[c.who.conn];
	++g->by_provider[c.tool.provider];
	c.holds_slot = true;
	c.high_impact = high_impact(c.tool);
	/* Recorded, synced, before it's sent. */
	if (c.high_impact && g->audit != nullptr)
		g->audit->write(line_for(c, "intent"), true);
	arm_timeout(c);
	c.deadline = g_timeout_add_full(G_PRIORITY_DEFAULT, static_cast<guint>(g->t.call_deadline.count()),
			on_deadline, new std::string(c.id), [](gpointer d) { delete static_cast<std::string *>(d); });
	std::string id = c.id;
	c.handle = providers_call(c.tool, c.args,
			[id](bool ok, const json::Object &body) {
				if (g == nullptr)
					return;
				auto it = g->calls.find(id);
				if (it == g->calls.end())
					return;
				Call &c = *it->second;
				bool lost = body.count("lost") != 0;
				if (body.count("not_sent")) {
					/* Never sent (its provider isn't there or ready): refused. */
					if (c.state != State::Ended) {
						if (c.sink.fail)
							c.sink.fail(-32603, body.count("message") && body.at("message").string()
									? *body.at("message").string() : "its provider isn't ready");
						c.sink = {};
						release(c);  /* (never sent: nothing holds it) */
						c.state = State::Ended;
						if (g->audit != nullptr) {
							json::Object line = line_for(c, "call");
							line["decision"] = "refused";
							line["outcome"] = "refused";
							line["why"] = "provider";
							g->audit->write(std::move(line), c.high_impact);
						}
					}
					/* (erased by calls_begin's caller path, or here) */
					g_idle_add([](gpointer d) -> gboolean {
						std::unique_ptr<std::string> id(static_cast<std::string *>(d));
						erase(*id);
						return G_SOURCE_REMOVE;
					}, new std::string(id));
					return;
				}
				if (c.state == State::Ended) {
					/* Late: after the agent was told. Recorded, not sent. */
					if (!lost && g->audit != nullptr) {
						json::Object line = line_for(c, "late");
						line.erase("args");
						line["ms"] = ms_since(c.received);
						g->audit->write(std::move(line));
					}
					erase(id);
					return;
				}
				if (lost) {
					/* Dispatched, and its answer won't come. */
					unknown(c, "the provider went away");
					erase(id);
					return;
				}
				json::Object extra;
				if (ok) {
					extra["result"] = json::Object{{"bytes", static_cast<double>(json::dump(body).size())}};
					if (c.sink.complete)
						c.sink.complete(body);
				} else {
					extra["error"] = json::Object{{"message", body.count("message") && body.at("message").string()
							? *body.at("message").string() : ""}};
					if (c.sink.fail)
						c.sink.fail(body.count("code") && body.at("code").number()
								? static_cast<int>(*body.at("code").number()) : -32603,
								body.count("message") && body.at("message").string()
										? *body.at("message").string() : "the call failed");
				}
				const char *outcome = c.state == State::CancellationRequested ? "cancellation-requested" : "completed";
				c.sink = {};
				end(c, outcome, "level", std::move(extra));
				erase(id);
			},
			[id](double progress, const json::Value &total, const std::string &message) {
				if (g == nullptr)
					return;
				auto it = g->calls.find(id);
				if (it == g->calls.end() || it->second->state == State::Ended)
					return;
				/* Heard from: CallTimeout starts again (CallDeadline doesn't). */
				arm_timeout(*it->second);
				if (it->second->sink.progress)
					it->second->sink.progress(progress, total, message);
			});
}

bool may_run(const Call &c) {
	return g->by_agent[c.who.conn] < g->t.max_in_flight &&
			g->by_provider[c.tool.provider] < g->t.max_in_flight;
}

void dispatch_waiting() {
	if (g == nullptr)
		return;
	for (auto it = g->waiting.begin(); it != g->waiting.end();) {
		auto c = g->calls.find(*it);
		if (c == g->calls.end() || c->second->state != State::Waiting) {
			it = g->waiting.erase(it);
			continue;
		}
		if (!may_run(*c->second)) {
			++it;
			continue;
		}
		it = g->waiting.erase(it);
		dispatch(*c->second);
	}
}

}  // namespace

std::string outcome_unknown_text(const std::string &why) {
	return "This call may or may not have taken effect: check before doing it again. (Its outcome "
			"is unknown: " + why + ".)";
}

void calls_init(const Tunables &t, Audit *audit) {
	calls_stop();
	g = new Calls;
	g->t = t;
	g->audit = audit;
}

void calls_stop() {
	Calls *c = g;
	g = nullptr;
	delete c;
}

void calls_set_locked(bool locked) {
	if (g != nullptr)
		g->locked = locked;
}

void calls_refused(const Caller &who, const OfferedTool &tool, const json::Object &args,
		const std::string &why) {
	if (g == nullptr || g->audit == nullptr)
		return;
	Call c;
	c.id = "c-" + std::to_string(g->next++);
	c.who = who;
	c.tool = tool;
	c.args = args;
	c.received = std::chrono::steady_clock::now();
	end(c, "refused", why.c_str());
}

std::string calls_begin(const Caller &who, const OfferedTool &tool, const json::Object &args,
		CallSink sink) {
	auto c = std::make_unique<Call>();
	/* An id the audit's lines share: unique for this hub, and across its
	 * restarts (a random part). */
	c->id = "c-" + std::to_string(g_random_int() & 0xffff) + "-" + std::to_string(g->next++);
	c->who = who;
	c->tool = tool;
	c->args = args;
	c->sink = std::move(sink);
	c->received = std::chrono::steady_clock::now();
	std::string id = c->id;
	/* Refused before anything's sent: while locked, past CallRate. */
	auto refuse = [&](const char *why, const std::string &text) {
		if (c->sink.fail)
			c->sink.fail(-32603, text);
		c->sink = {};
		end(*c, "refused", why);
	};
	if (g->locked) {
		refuse("locked", "the screen is locked: nothing is read or done until it's unlocked");
		return id;
	}
	auto &recent = g->recent[who.conn];
	auto now = c->received;
	while (!recent.empty() && now - recent.front() > std::chrono::minutes(1))
		recent.pop_front();
	if (recent.size() >= g->t.call_rate) {
		refuse("rate", "too many calls: at most " + std::to_string(g->t.call_rate) +
				" a minute (CallRate)");
		return id;
	}
	recent.push_back(now);
	Call &ref = *c;
	g->calls[id] = std::move(c);
	if (may_run(ref) && g->waiting.empty())
		dispatch(ref);
	else
		g->waiting.push_back(id);
	return id;
}

void calls_cancel(const std::string &id) {
	if (g == nullptr)
		return;
	auto it = g->calls.find(id);
	if (it == g->calls.end())
		return;
	Call &c = *it->second;
	c.sink = {};  /* (no answer goes to an agent that cancelled) */
	if (c.state == State::Waiting) {
		end(c, "cancelled", "agent");
		erase(id);
	} else if (c.state == State::Dispatched) {
		c.state = State::CancellationRequested;
		providers_cancel(c.handle);  /* (asked to stop: its answer still comes) */
	}
}

void calls_agent_left(unsigned conn) {
	if (g == nullptr)
		return;
	std::vector<std::string> ids;
	for (auto &[id, c] : g->calls)
		if (c->who.conn == conn)
			ids.push_back(id);
	for (const std::string &id : ids)
		calls_cancel(id);
	g->recent.erase(conn);
}

}  // namespace wb::agent
