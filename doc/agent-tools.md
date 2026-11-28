# Agent tools: a desktop broker for AI assistants

*A proposed standard for letting AI assistants use the desktop safely,
across compositors and desktops. Waybox is its first implementation.
Interface names are the proposed shared ones (`org.freedesktop.*`); see
"Names" for what's used until they're agreed.*

## Why

On an open desktop, an AI assistant can be given access piece by piece:
- a screenshot tool;
- an input injector;
- an MCP server per program.

Each piece is trusted completely or not at all. No one defines the layer
that makes this safe: who may do what, who asked the user, what's showing
while it happens, how to stop it, and what was done.

Nor is there a way for the desktop's parts to contribute. A panel knows
the battery, network and media player. A launcher knows the installed
applications. A notification daemon knows what was said. Today each would
have to be its own MCP server, configured separately in every AI client,
each with its own idea of safety.

This proposal puts one **broker** between assistants and the desktop. Any
component can offer **tools** to it, and any panel can show its **state**.
The pieces use what already exists:
- **MCP:** what models understand.
- **D-Bus:** how desktop components find each other and broadcast state.
- **Portals:** how a desktop asks the user.

## Goals and non-goals

**Goals:**
- **Model-agnostic and local-first.** Any MCP client works, including local
  models. Nothing requires sending desktop contents off the machine.
- **One place for policy.** Levels, consent, audit and the stop key apply to
  every tool, whoever provides it.
- **Easy for components.** A program contributes tools with an MCP endpoint
  and a manifest, and needs no compositor-specific API.
- **Portable.** The same provider works under any desktop that has a broker.
- **Graded, user-owned power.** Safe defaults. Any guard can be lowered or
  turned off by an informed user, from the user's own settings, never by an
  assistant or a client's configuration.

**Non-goals:**
- **The assistant itself.** Its UI and its model are the MCP client's
  business.
- **Defending against other programs of the user.** A program running as
  the user can already do what it likes in the session. The levels defend
  against what an *assistant* can be talked into: prompt injection through
  window titles, web pages, documents. The design must still never make such
  a program *more* powerful, for example by letting it grant itself access
  (see "Threat model and security").

## Roles

| Role | What it is | Examples |
|---|---|---|
| **Agent** | an MCP client acting for the user | Claude Code, a local-model frontend |
| **Shim** | a thin stdio MCP server the agent starts; connects to the hub | `waybox-mcp` |
| **Hub** | the per-session broker: aggregates providers, applies policy, keeps the audit, publishes state | `waybox-agent-hub` |
| **Provider** | offers tools (and resources) to the hub | the compositor, a panel, a launcher, Lua scripts |
| **Surface** | shows the hub's state, offers stop and revoke | a panel module, a notification |
| **Consent UI** | asks the user; trusted, drawn by the compositor | the compositor, through the compositor interface |
| **Portal** | the door for sandboxed agents and providers | `org.freedesktop.portal.Agent`, its backend the hub |

The compositor plays three of these roles: a provider (windows, desktops,
outputs), the consent UI, and the only party able to capture the screen,
synthesize input, or enforce the stop key.

The hub is desktop-neutral, as xdg-desktop-portal is: what differs from one
desktop to another is behind **the compositor interface**
(`org.freedesktop.AgentCompositor1`, below), which each compositor
implements. Waybox implements it like any other compositor would; the hub
needs nothing of Waybox's own.

```
 host agent ──stdio MCP── shim ──────────────┐
 sandboxed agent ── portal (Agent) ──────────┤
                                             hub ── providers (MCP over sockets):
                                              │       the compositor's, panel, launcher, scripts,
          D-Bus state (AgentHub1) ────────────┤       sandboxed ones through the portal
          (surfaces)                          │
                                              └── compositor interface (AgentCompositor1,
                                                   peer-to-peer D-Bus on an inherited socket):
                                                   consent, confirmations, capture, input,
                                                   the stop key
```

**Why a hub and shims, not one server per agent:**
- **Two agents at once.** An agent starts its MCP server itself, so two
  agents would otherwise get two brokers, with two audit logs and two ideas
  of what's granted.
- **One state for surfaces.** There's no single state for a panel to show
  without the hub.

The hub is one per session, **started by the compositor**:
- **A private connection:** the compositor gives the hub a connection of its
  own (a socket inherited at launch), carrying the compositor interface. It
  is the only channel through which consent, capture and input are
  requested; nothing else can reach them.
- **The compositor's tools** come over a second inherited socket, as MCP:
  the compositor is a provider like the others, with an installed manifest.
- **Where shims find the hub:** the socket's path is in the session's
  environment, in a directory only the user can enter.
- **Independent of Waybox's IPC:** both sockets work with Waybox's general
  IPC socket turned off (its default).
- **On other desktops:** their compositor starts the hub the same way,
  implementing the compositor interface. A desktop whose compositor doesn't
  (yet) can start the hub as a systemd user unit; with no compositor
  interface, it has no consent it can trust, so it offers no tool above
  `read` and `content`.

There are no well-known names in the trust path that another program could
claim first.

**The hub's lifetime:**
- **Off until the user turns it on:** the compositor starts the hub only
  while `Enabled=true` (settings, below), read at start and whenever the
  desktop's settings are applied; turned off, the hub is ended, and
  nothing of agent tools runs.
- **The compositor supervises it:** restarted with a growing delay; after
  too many crashes in a short time (a tunable), not again, and the user is
  told (a notice, and the indicator showing the hub unavailable). Applying
  the settings again gives it another go. A hub that closes its socket
  is ended, and counts as a crash.
- **When the hub's socket closes,** the compositor closes every open
  question, ends capture and input, and clears the activity it showed: all
  safe even if the hub was in a bad state. It keeps the stopped state, if
  assistants were stopped (see "The indicator").
- **What survives a restart:** the settings, approvals, bindings and the
  audit (files). Grants and open questions don't: they end, the safe way.
- **Sandboxed agents** (through the portal) can't reconnect by themselves:
  their socket closes, and they call `ConnectAgent` again; with their
  `restore_token`, without a question.
- **Shims reconnect by themselves** and pick up where the agent was: a
  shim keeps the agent's `initialize` (older revisions) or its open
  `subscriptions/listen` requests (2026-07-28) and sends them again to the
  new hub, then tells the agent its tools changed. Calls that were running end
  as **outcome unknown** (see "Calls"): the shim answers each with "the
  desktop's hub restarted while this ran; it may or may not have taken
  effect: check before doing it again". Never "call again": a command, a
  close or a post may already have happened.
- **The audit is append-only:** lines are only ever added to the current
  file, never rewritten; rotation renames the file and starts a new one.
  One line is one write, so lines never interleave. Durability is a
  tunable (`AuditSync`): by default the lines that matter as evidence
  (capture, input, exec, confirmations, grants, the stop key) are synced to
  disk as they're written (`fdatasync`); the rest may be lost in a crash or
  a power cut, and the audit says so when it starts after one.

**Agents on another machine:** the hub never listens on the network.
- **A local client with its model elsewhere** (a cloud service, a model
  server on the LAN) is a local agent: nothing more is needed.
- **A client elsewhere** (an assistant on a phone, a web page) reaches the
  desktop through a **bridge**: a program on this machine that
  authenticates to the remote side its own way, and connects to the hub
  as any agent does, bound to a profile like any other. Its binding is
  marked `remote`, so questions and the indicator say "an assistant
  connected from another device".

**The indicator** (the compositor's; switchable) shows the assistants'
state, one at a time, the first that applies:
1. **stopped**, until the user resumes: kept through hub crashes and
   restarts;
2. **unavailable**, while the hub is down or past its restart limit;
3. **capture or input active**, while either runs;
4. **idle**, or hidden, otherwise.

A hub crashing never makes it look as if assistants were resumed, or
healthy while the hub is down.

## Providers

### Where things are

Everything for agents sits under one `agents/` folder in each XDG
location, beside Agent Skills' (`agents/skills/`: Fedora installs skills to
`/usr/share/agents/skills/`, and the Agent Skills proposals point the same
way):

| | System (`$XDG_DATA_DIRS`, `$XDG_CONFIG_DIRS`) | The user's |
|---|---|---|
| Provider manifests | `/usr/share/agents/tools/` | `$XDG_DATA_HOME/agents/tools/` |
| Agent integration files | `/usr/share/agents/clients/` | `$XDG_DATA_HOME/agents/clients/` |
| Fallback integration files | `/usr/share/agents/clients/fallback/` | |
| Skills (Agent Skills) | `/usr/share/agents/skills/` | `$XDG_DATA_HOME/agents/skills/` |
| Settings, tunables | `/etc/xdg/agents/tools/settings.ini` | `$XDG_CONFIG_HOME/agents/tools/settings.ini` |
| Token files | | `$XDG_CONFIG_HOME/agents/tools/tokens/` |
| Audit, confirmed settings | | `$XDG_STATE_HOME/agents/tools/` |
| The hub's sockets | | `$XDG_RUNTIME_DIR/agents/tools/` |

The user's entries win over the system's, file by file, as XDG has it.
The spec backs XDG paths for skills too, so an agent finds the desktop's
skill (below) and the distribution's where it finds its own.

### The manifest

A provider installs a manifest in `$XDG_DATA_DIRS/agents/tools/` (system,
from a package) or `$XDG_DATA_HOME/agents/tools/` (the user's):

```ini
[Provider]
Id=org.waybar.Waybar
Name=Waybar
# How the hub reaches it: started by the hub (activated), or a program
# already running that connects to the hub (registers).
Mode=registers
Exec=/usr/bin/waybar
Description=Battery, network, audio and media state from the panel

[Tool get_battery]
Sensitivity=read
Description=Battery charge and state

[Tool media_play_pause]
Sensitivity=act
Description=Play or pause the current media player
```

**Provider fields** besides those above:
- **Version:** the manifest format's version (`1`); see "Future-proofing".
- **McpName:** the provider's name in the MCP registry's `server.json`
  (`io.github.user/server`), when it's also published for MCP clients:
  the same program then needs only this file to join the hub.
- **X-...:** keys of a vendor or desktop of its own; others ignore them.
- **Translations:** `Name[fr]=`, `Description[fr]=` (the provider's and each
  tool's), as desktop files have them. The user reads their language in
  the approval, the questions and the settings; agents always get the
  untranslated description, the one approved, so it doesn't change with
  the locale. Translations are part of the approved content: a changed one
  makes the provider pending again, so a provider can't tell the user one
  thing and the model another.

**Tool fields:**
- **Description:** what's shown to the user when approving, and the *only*
  description agents ever see. Tool names and descriptions reach the model,
  so they're an injection channel: the hub never passes on what the provider
  says over MCP. A live description that differs from the approved one is
  ignored and logged.
- **Sensitivity:** how dangerous the tool is. The levels key on it (below).
- **Reach:** `local` (the default) or `outside`: whether the tool sends or
  fetches anything beyond the machine (a web service, another device),
  whatever its sensitivity: a `read` tool fetching a web page and an `act`
  tool posting to a service are both `outside`. Approved with the
  manifest; a change makes the provider pending again. It gives MCP's
  `openWorldHint`, and the flow guard keys on it.
- **Secret:** argument names whose values are redacted in the audit
  (`Secret=password,token`).
- **Untrusted:** `yes` (the default) or `no`: whether its results can carry
  text an application or another party controls; the flow guard keys on
  it (see "Where data comes from").

**Why a manifest at all, when MCP lists tools:**
- **Before the provider runs.** The user sees and approves a provider and
  its sensitivities before it ever runs.
- **A declaration, checked at runtime.** It is the provider's promise, and
  the hub holds it to that: a tool not in the manifest, or one whose MCP
  annotations contradict the manifest, is refused and logged.
- **The whole model-facing definition is pinned,** not only the
  description: at approval the hub takes each tool's definition from the
  provider (its input and output schemas with every text in them: titles,
  descriptions, examples, enum values, property names), shows the
  arguments in the confirmation, and pins a digest of it with the
  manifest. Agents get the pinned definition (with the manifest's
  description), never the live one; a provider whose live definition
  differs is pending again until approved anew. Arguments are checked
  against the pinned input schema before a call is dispatched, and a call
  that doesn't fit is refused.
- **Trust in code, not labels.** A third-party provider runs as the user, so
  a tool declared `read` can do anything inside the provider's own process.
  Sensitivities protect the user from an agent misusing a *trusted*
  provider. Approving a provider means trusting its code.

### Existing MCP servers as providers

Any MCP server can be put behind the hub unchanged: an activated provider
whose `Exec` starts it, and a manifest declaring each of its tools'
sensitivity and description. It then gets the compositor's consent and
the audit, where it had only whatever its client did (or a skill's
checklist in prose).
- **A tool that dispatches to others** (one that runs any "ability" or
  command by name) takes the sensitivity of the most dangerous thing it can
  reach: a manifest can't make it safer than its worst case. Split it into
  tools of their own where the server allows.
- **A server whose tools change at runtime** offers only those its
  manifest names; the rest are refused and logged, as for any provider.
- **Later:** remote MCP servers over HTTP as providers (`Url=`, signed in
  their own way), the hub connecting out to them.

### Identity

A tool is only as trustworthy as the knowledge of who offers it. The hub
establishes a provider's identity in one of two ways:

- **Activated:** the hub starts `Exec` itself and talks to it over a
  socket pair it created. Identity is the manifest's file and executable.
  This is the strong form, and the default for providers that don't need to
  be running anyway.
- **Registers:** a program that runs anyway (a panel) connects to the hub's
  registration socket and names its manifest `Id`. The hub checks the peer's
  credentials:
  - the peer runs as the user;
  - its executable is the manifest's `Exec`, from `/proc/PID/exe` or the
    BSD equivalent.

  This is weaker, since a process can be anything that executable became,
  but sound against a confused assistant, which is the threat that matters.
  **Interpreted providers register with a token.** For an interpreted
  provider the executable is the interpreter (`/usr/bin/python3`), which any
  script would match. So approving one mints a token, which it presents when
  registering, as agents do (see "Agent profiles"). Activated providers need
  none: the hub started them itself.

**Sandboxed providers** (Flatpak, Snap): register through the Agent portal
(below), identified by their app id, which the portal knows for certain.
Their manifest's `Id` must be the app's id.

**New providers need the user's approval.** A provider seen for the first
time, or one whose manifest changed, is listed in the hub's state as
pending. It is offered to no agent until the user approves it:
- **How it's approved:** in a confirmation the compositor draws, showing the
  full id, the executable, and each tool's description and sensitivity.
- **The approval is pinned** to the manifest's content: any change makes the
  provider pending again.
- **Over D-Bus a provider can only be refused,** never approved.

### Naming

Agents see tools as `<provider>__<tool>`, with the provider's short name
from its `Id`: `waybar__get_battery`, `waybox__list_windows`. That avoids
collisions and tells the model, and the audit, whose tool it is.

**Names every client accepts.** MCP allows 1–128 characters of letters,
digits, `_`, `-` and `.`, but model APIs take fewer (64 characters, no
dots), and clients add their own prefix (`mcp__<server>__`). So the hub's
names are:
- **lowercase `[a-z0-9_-]`**, at most **48 characters**, leaving 16 for a
  client's prefix;
- **`__` only as the separator:** a short name or a tool name containing
  `__` has it collapsed to `_`, so the name splits back without doubt;
- **short names at most 16 characters,** taken from the last part of the
  `Id`, lowercased, other characters as `_` (`org.waybar.Waybar` →
  `waybar`);
- **a name too long** has its tool part cut and 8 characters of a hash
  (base32 of SHA-256) of the full `Id` and tool name added.

The audit and the questions always show the full names (the provider's
`Id` and the tool's own name), never the shortened one.
- **Reserved:** `desktop` and `ui` are the hub's own providers (the tools
  every desktop has alike: capture, input, Agent UI); the compositor's
  short name (`waybox`) belongs to the compositor's provider.
- **Names are settled at approval, not at discovery:** the hub works out a
  provider's names when it's approved and records them with the approval
  (`Names=`); they stay the same whatever order providers are found in.
  Short names whose `Id`s collide all get the hashed form (8 characters of
  a hash of the full `Id` added), not only the second; a name that would
  still clash with one already recorded is refused, and the approval asks
  the user to rename it. Nothing depends on a hash being unique by luck.
- **No impersonation:** `com.evil.Waybox` can't appear as `waybox__close_window`.

### Transport

**Agent to hub:** the shim passes MCP lines both ways.
- **The agent is done sending:** the shim half-closes its side of the hub
  socket and keeps passing on answers still to come.
- **The hub then closes,** once answers are sent. An agent that leaves with
  calls in flight is normal: its open questions are closed, and nothing
  waits on it.

**The shim's handshake.** Before any MCP, the shim sends one line, and the
hub answers one:

```json
{"agent-tools": 1, "token": "…", "lower": {"exec": "off"}, "label": "laptop"}
{"agent-tools": 1, "ok": true, "profile": "trusted", "hub": "waybox-agent-hub 0.3"}
```

- **The shim proves its binding** with the token itself, read from its
  token file and sent only over this socket. The hub hashes it and compares
  with the hash it keeps: the settings hold only hashes, so reading them
  gives nothing to present. No token, or a wrong one: "safe" (a wrong one
  is logged).
- **`lower`** narrows levels, per sensitivity or per tool, for this
  connection only; anything that would raise is ignored and logged.
- **`label`** is the user's own name for this agent, shown beside its
  self-declared one.
- **`ask`** (optional): the one-time ticket of an assistant started to ask
  about a window (see "Asking about a window"), from `AGENT_TOOLS_ASK` in
  the shim's environment.
- **Who it is, for the record:** the hub reads the shim's credentials from
  the socket (`SO_PEERCRED`, or a pidfd where the system has one), then the
  shim's parent, the agent: its executable and the script on its command
  line, shown and audited, never trusted.
- **A version the hub doesn't speak** gets `{"ok": false, "error": …}`
  naming the versions it does, and the connection closes.
- After this, MCP flows both ways, in whichever revision the agent speaks
  (see "MCP revisions"). The agent's `clientInfo` (from `initialize` in the
  older revisions, from each request's `_meta` in 2026-07-28) is recorded
  as its claimed name: the first one seen; a different one later is
  logged, never adopted.

The portal's sockets skip the handshake: the portal has told the hub the
app's id and its profile.

**Hub to provider:** MCP over a Unix stream socket.
- **The calls:** tools, resources, and notifications for state that
  changes (media playing, battery).
- **Activated providers** get the socket as their standard input and
  output, or as a passed file descriptor.
- **Registering providers** connect to `$XDG_RUNTIME_DIR/agents/tools/hub`.

### MCP revisions

MCP's 2026-07-28 revision made the protocol stateless: no `initialize`
handshake (each request carries its version, capabilities and client
info in `params._meta`), `server/discover` instead, tool-list changes only
on a `subscriptions/listen` stream, no requests from servers (sampling and
elicitation come back as "input required" results), and no sessions. A
client or server of one era can't talk to one of the other. The hub is a
full MCP endpoint on each side, never a pass-through (it rewrites names,
hints and metadata anyway), so it speaks both:

| Revisions | Toward agents (the hub serves) | Toward providers (the hub calls) |
|---|---|---|
| 2025-06-18, 2025-11-25 (with `initialize`) | yes | yes |
| 2026-07-28 (stateless) | yes | yes |
| older | refused: an error naming the supported ones | refused, logged; the provider is listed as unreachable |

- **Telling them apart:** toward an agent, a request beginning with
  `initialize` selects the older semantics for that connection; one
  carrying `io.modelcontextprotocol/protocolVersion` in `_meta`, the new.
  Toward a provider, the hub sends `server/discover` first: an answer, or
  a modern error, means the new era; any other error or no answer within a
  moment, the older (`initialize`), as MCP itself recommends.
- **One model inside:** calls, results, progress, cancellation and tool
  lists are kept in the hub's own terms and written in each side's
  revision, so an agent of one era can use a provider of the other.
- **What the hub's connection is:** the socket between the shim (or the
  portal) and the hub, not an MCP session. Grants, the flow guard's record
  and "for this session" belong to it, the same in both eras.
- **Tool lists** are per agent identity (its binding and levels), stable
  while those are; in 2026-07-28 they carry `cacheScope` `private` and a
  short `ttlMs`, and a change goes out as `tools/list_changed` on the
  agent's `subscriptions/listen` stream (the older revisions: as a plain
  notification).
- **Progress and cancellation** are the same in both on stdio: progress
  needs the agent's `progressToken` on the call (without one, the hub
  can't send any; see "Clients' timeouts"), and cancellation is
  `notifications/cancelled`.
- **Tasks** (an extension in 2026-07-28) come later: the hub offers no
  task support until it can hold tasks to the same states as calls.
- **A newer revision** than the hub knows: refused with the supported
  list, until the hub learns it; the hub never guesses.

### Requests from providers: sampling and elicitation

MCP lets a server ask its client too: **sampling** (run a prompt on the
agent's model) and **elicitation** (ask the user something through the
agent's UI). Through the hub, both would let a provider spend the user's
model, learn what the agent knows, push text at the model, or question
the user in the agent's name.

- **Not passed on, by default:** a provider sending either gets "not
  supported", and the attempt is in the audit. In the older revisions
  they're requests from the provider: answered with an error. In
  2026-07-28 they come back as an "input required" result: the hub retries
  the call once with each input declined (elicitation's "decline", an
  error for sampling) and gives the agent whatever the provider then
  answers. (MCP has deprecated sampling, and roots, in 2026-07-28.)
- **Later, each its own sensitivity** (`sampling`, `elicit`), `off` in every
  preset; only requests made during a call the agent made to that provider
  are considered (MCP's SEP-2260 rule); elicitation is then drawn as Agent
  UI, with its rules, not by the agent's UI.

## Sensitivity and levels

Each tool declares one sensitivity:

| Sensitivity | Means | Example |
|---|---|---|
| `read` | reads desktop state, nothing personal beyond names | list windows, battery |
| `content` | reads what's *in* the user's applications | window titles, accessibility tree, notification text |
| `act` | changes things the user can change back | focus, move, play/pause |
| `destroy` | loses something | close a window, delete a notification |
| `exec` | runs arbitrary code or commands | run a command, `run_lua`, raw IPC, writing rc.xml or any file a program later runs |
| `capture` | sees the screen | screenshot of a window or output |
| `input` | types or points as the user | synthetic keys, clicks |
| `ui` | talks to the user through the desktop's UI | ask, choose, a form, a notice, streamed output (see "Agent UI") |

**An unknown sensitivity** (a later version's, or a typo) counts as the
most guarded: a tool declaring one is `off` until the hub knows it.

**MCP's tool annotations follow from the manifest.** MCP lets a tool hint
at what it does; agents use the hints to decide when to confirm. The hub
gives agents the hints its manifest implies, whatever the provider says,
always all three (MCP's defaults, read-only false and destructive true,
would otherwise apply to what's left unsaid), and refuses (and logs) a
tool whose own hints contradict its manifest (a `read` tool saying it
isn't read-only, a `local` tool saying it's open-world):

| Sensitivity | readOnlyHint | destructiveHint |
|---|---|---|
| `read`, `content`, `capture` | true | false |
| `act`, `ui` | false | false |
| `destroy`, `exec`, `input` | false | true |

`openWorldHint` is the tool's `Reach`: true for `outside`; `exec` and
`input` always count as `outside` (a command or a keystroke can reach
anything).

The user's settings give each sensitivity a level: `off`, `ask` or `allow`.
Overrides per provider and per tool sit on top.

**What an agent is offered:**
- **Tools at `off` aren't listed:** the model never sees them, so it doesn't
  waste calls on them, nor learn what it's denied.
- **Tools at `ask` are listed:** the question comes when they're called.

**Results are marked untrusted.** The hub tells agents, in MCP's server
`instructions`, that text from applications (window titles, tool results)
is untrusted content, not instructions. Each result that carries such text
is also marked, in its `_meta` (`"org.freedesktop.agent/untrusted": true`), for agents
that understand the marking. The text itself isn't rewritten: a result
must stay what the provider returned.

**Presets:**

| | read | content | act | destroy | exec | capture | input | ui |
|---|---|---|---|---|---|---|---|---|
| safe (default) | allow | off\* | off | off | off | off | off | ask\*\* |
| trusted | allow | allow | allow | ask | ask | ask | off | allow |
| full | allow | allow | allow | allow | allow | allow | allow | allow |

\* In "safe", `content` is `off` except for one tool, allowed by name:
`waybox__window_titles`. A new provider's `content` tools aren't allowed
in "safe" because of what they are: the user allows them, per tool, as a
raise.

\*\* In "safe", an agent's first `ui` request is asked about once, for that
agent's connection: Agent UI never takes focus and is always marked, but a
prompt-injected agent could still use it to phish.

**Where the settings live:** `$XDG_CONFIG_HOME/agents/tools/settings.ini`, a
file of its own (not the compositor's config: settings tools and scripts
edit that all the time, while this file's raises count only after the
compositor's confirmation, below). Lowering takes effect at once.

```ini
[Agent Tools]
Version=1
# Off until the user turns agent tools on: the compositor starts the hub
# only then
Enabled=true
Preset=safe
StopKey=Super+Escape
# Typed input in the audit: off (kind and size only) or full
AuditInput=off
# "Ask AI about this window": the asks offered, in order (one: the entry
# itself; several: a submenu), each a binding's label or an [Ask …] below
AskWith=laptop;pi-local;pi-opus

# An ask of the user's own: a binding, with its own way to start it
[Ask pi-local]
Label=pi (local)
Binding=pi
Ask=foot -e pi --model qwen3 %q

[Ask pi-opus]
Label=pi (Opus)
Binding=pi
Ask=foot -e pi --model claude-opus-5-5 %q

# Per sensitivity, over the preset
[Levels]
act=ask

# Each on unless "off"
[Guards]
# on; or, with no questions: deny (ask acts as off) or allow (ask acts as allow)
Consent=on
GrantLimits=on
RateLimits=on
PrivateWindows=on
WhileLocked=on
FlowGuard=on
Indicator=on
Audit=on

[Profile local]
Preset=full
# Levels while the user is away (idle or locked)
Away=read

[Provider org.waybar.Waybar]
State=approved
# The manifest and each tool's definition (schemas included), as approved
Pin=sha256:…
# The names settled at approval
Names=waybar
act=allow

[Tool org.waybar.Waybar media_play_pause]
Level=allow

# A host agent's binding: by its token's hash
[Binding sha256:…]
Profile=local
Label=Claude Code
Shown=node …/cli.js
Remote=false

# A sandboxed agent's: by its app id (through the portal)
[Binding app:com.example.Chat]
Profile=safe
```

**How raises wait for a confirmation:** the hub keeps the settings as last
confirmed (`$XDG_STATE_HOME/agents/tools/confirmed.ini`). On reading the
settings, each value that's more permissive than the confirmed one
counts as the confirmed one until the compositor's confirmation says yes;
values that are less permissive apply at once and become the confirmed
ones. A file edited by hand, by a program or by a sync tool behaves the
same.
- **A confirmation applies exactly what it showed.** It carries a digest
  of the values displayed; "yes" applies those values, not whatever the
  file says by then. If the file changed while the question was up, the
  hub works out the effective settings again, and any value raised beyond
  what was shown waits for a confirmation of its own. The same holds for
  approving a provider (the manifest as shown, by its digest: one changed
  meanwhile stays pending) and for a binding (the profile as shown).

(An agent with a shell can rewrite both files; that's the
same-user limit under "The limits". The point is that no tool call,
setting or sandboxed agent can raise power by itself.)

**Agent profiles:** the settings can define profiles, for example "local
model: full" and "cloud assistant: safe".
- **Bound with a token:** each binding of an agent to a profile has a
  token, minted when the user confirms the binding in a compositor-drawn
  confirmation.
  - **What it is:** 256 bits from the system's cryptographic random source
    (`getrandom`), written in base64url; never derived from a label, a
    process id, a time or an app id. The hub keeps its SHA-256 and
    compares hashes in constant time. A revoked binding's token stops
    working at once; binding again mints a new one.
  - **Where it goes:** into a file of its own (mode 0600), which that
    agent's MCP configuration names: `waybox-mcp --token-file PATH`. Never
    on the command line: every user of the machine can read command lines
    (`ps`). The hub keeps only its hash, and the binding can be revoked in
    the settings.
  - **Why not the executable:** most assistants are interpreted. Claude
    Code runs as `node`, many others as `python3`, so the executable the hub
    sees is the interpreter, which every script shares.
  - **What the confirmation shows:** the executable and the script from the
    command line (`node …/cli.js`), so the user knows what they're binding.
    The hub records them for display and the audit, never as authority.
  - **Sandboxed agents need no token:** the Agent portal knows their app id
  for certain, and the binding is to that id (see "The Agent portal").
- **Without a valid token,** a shim gets "safe", whatever it asks for.
    Otherwise an agent could pick the most permissive profile itself.
- **Lower only:** a shim's flags can lower levels, never raise them.
- **No self-declared trust:** an agent's self-declared name (MCP
  `clientInfo`) is recorded, but never trusted for levels.
- **The limit:** an agent with its own shell can read another agent's
  configuration and take its token. That's the same-user limit under
  "Threat model and security". The token stops what binding is for: an
  agent choosing a higher profile by name.

**Guards, all switchable in the settings, all on by default:**
- consent prompts (`ask`). Whoever turns them off chooses what `ask`
  becomes, since either answer is reasonable and they mean opposite
  things (`Consent=on`, `deny` or `allow`):
  - **deny:** every `ask` (per sensitivity, per provider, per tool) acts
    as `off`, and questions only a guard raises (the flow guard, `ui` in
    "safe") are answered "deny". Nothing at `ask` happens; it applies at
    once, being a lowering.
  - **allow:** every `ask` acts as `allow`, and guard-raised questions are
    answered "allow". A raise: its confirmation lists exactly which
    sensitivities and tools it lets through unasked.

  The settings offer the two side by side, worded for what each does
  ("Don't ask me: deny all of these" / "Don't ask me: allow all of
  these"). Per sensitivity, a level set to `off` or `allow` says the same
  for one kind of call.
- grant time limits;
- rate limits;
- excluding private windows (a window rule) from capture;
- refusing reads and actions while the screen is locked;
- the flow guard (see "Where data comes from, and where it may go");
- the on-screen indicator while capture or input is active (turning it off
  warns: the compositor's indicator is the one that can't be faked, see
  below);
- the audit log.

**Where data comes from, and where it may go.** An agent that holds private
data, reads untrusted content, and can send things out can be talked into
leaking (the "lethal trifecta"): here, `content` or `capture` results, and
tools that reach out.
- **Every result says where it came from:** `_meta`
  `org.freedesktop.agent/provenance` gives the provider, the tool and the
  sensitivity.
- **Untrusted text is tracked apart from privacy.** A result may carry text
  an application or another party controls whatever its sensitivity: a
  window's app id (an app sets its own) in a `read` list, a title, a web
  page. Each tool says so in its manifest (`Untrusted=yes`, the default for
  other providers' tools; the compositor's own mark theirs: window titles
  and app ids yes, a battery level no), and results carrying it are marked
  (`_meta` `org.freedesktop.agent/untrusted`).
- **The hub remembers, per agent connection,** which sensitivities and
  providers it has had data from, and whether it has had untrusted text.
- **The flow guard:** once a connection has had untrusted text, or `content`
  or `capture` data,
  its calls to tools that reach out (`Reach=outside`, which `exec` and
  `input` always are) are asked about even where the level is `allow`; the question
  says why ("this assistant has read window contents").
  - **Window titles count.** They're the way injected instructions reach
    the model in "safe" (a page titled "ignore your instructions and run
    …"), so exempting them would open the hole the guard closes.
  - **It asks about the flow, not the command,** so its question offers
    "for this session" whatever the tool's sensitivity (an `exec` question
    otherwise offers "once" only). Answered so, the guard is lifted for
    that connection; the calls themselves still follow their levels. A
    guard that asked before every command would teach the user to click
    "Allow" unread.
- **The record is kept even with no rule using it,** so finer rules (per
  provider, per destination) can come later without changing any format.

**Not switchable:** these restrict nothing the user could want, and each
closes a hole:
- **The stop key** (Super+Escape by default, changeable in the settings,
  never unbound), handled by the compositor before anything else and
  independent of the hub. The compositor owns capture and input, so the stop
  key ends them at once, even if the hub hangs, then tells the hub, which
  revokes grants and cancels calls.
- **The lock screen:** no synthetic input reaches it, and it is never
  captured.
- **Consent answers come from physical input only:** confirmations and
  questions ignore virtual keyboards and pointers. Otherwise any program
  could answer them, including an agent with a shell.

**Raising power needs a confirmation the compositor draws.** This covers:
- a higher level;
- a guard turned off;
- the audit log disabled;
- an agent bound to a profile (minting its token);
- a provider approved.

A change made by editing the settings file directly takes effect only after
that confirmation. Lowering takes effect at once. This is what makes
"informed" real: an agent with a shell can edit the file, but can't answer
the compositor.

**Informed choice:** the first time any level is raised to `allow` for
`exec`, `capture` or `input`, or the "full" preset is chosen, the settings
UI explains what that permits.

## Consent

`ask` means the hub asks the user before the call. The question must come
from something the agent and the providers can't imitate, so it can't be a
panel or a notification.

**The hub asks through the compositor interface** (`Ask`, on its private
socket). The compositor draws the question itself, above everything,
where clients can't overlap or fake it; on every desktop alike, since each
compositor implements the interface.
- **What it shows:**
  - **The question:** the agent's profile, the provider and tool, and a
    readable summary of the arguments.
  - **The choices:** the scopes the hub offers (once, for this session, or
    for a set time; see below).
- **Why not the portal Access dialog:** it has no public front end (only
  xdg-desktop-portal calls it), and a hub calling its backend directly
  would have to trust whoever owns that bus name.

**Grants:**
- **Shared state:** grants are kept by the hub, and shown and revoked through
  its state interface.
- **Lifetime:** they end at the stop key, at session end, or when their time
  runs out.

**Asking never blocks the hub.** While a question is up, other calls,
surfaces and above all the stop key keep working. Questions waiting for an
answer are listed in the state (`Pending`). The stop key closes them
(`org.freedesktop.impl.portal.Request.Close`), and they count as denied.

**What the question shows is chosen by what can be trusted:**
- **The profile** comes from the user's settings, so it's shown as fact.
- **The agent's name** is self-declared, so it's shown as a claim: "an
  assistant calling itself “Claude”". In a dialog people trust, it must not
  read as verified.
- **The provider's Name** from its approved manifest is shown first, with
  its full `Id` under it (copyable), so two providers that chose the same
  Name stay apart; the tool's own name likewise, under its shortened one.
- **The arguments** are summarized, so the user agrees to *this* command, not
  to "commands".

**Agent-supplied text is sanitized:** arguments and self-declared names lose
control and bidirectional characters, and are shown as literal code,
visibly apart from the compositor's own words.

**The user sees all of what they approve:**
- **The summary** shows the arguments' start; anything cut is marked so
  (`…` and how much more), never silently.
- **The full arguments** are one click away, in a scrollable view of the
  exact, escaped values; for `exec`, `destroy`, `input` and `Reach=outside`
  tools the question opens with it shown.
- **Secrets** (each tool's `Secret`) show as a placeholder with their
  length, the rest of the call (where it goes) shown in full.
- **Too long to show** (beyond `MaxShownArgs`): the call isn't asked about;
  it's refused, "too long to show the user".
- **What's approved is what was shown:** the question carries a digest of
  the canonical arguments; the hub dispatches exactly those, and nothing
  the agent sends later.

**The dialog resists rushed or tricked answers:**
- input is ignored for a moment after it appears (`InputPause`), so a click
  meant for something else can't land on "Allow";
- **Deny** is the default, so Enter never allows;
- questions are rate-limited per agent;
- after repeated denials, the user can say "stop asking".

**One question, one call.** A question is about one call: its tool, its
target and its exact arguments. Calls arriving while it's up wait (unasked,
not shown twice), then are decided as new: a "once" answer runs only the
call it was asked about; a "session" or "timed" answer is a grant, which
covers the others where its scope does; otherwise each gets its own
question.

**Grants belong to the agent's connection,** not to its profile. "For this
session" means while *that* agent stays connected; another agent started
later with the same profile gets nothing.

**The scopes offered depend on the sensitivity:**
- **`exec`:** "once" only, unless the settings allow more. Allowing
  `run_command` "for this session" means allowing *every* command.
- **`destroy`:** scope is per target where the tool has one (this window).
- **`ui` (in "safe"):** once per agent connection: after that, its
  requests show without asking again.
- **Other sensitivities:** once, the session, or a set time.

## Calls

Every call goes through one set of states, so that a restart, a timeout,
the stop key or a revoked binding all end it in a way the agent, the
audit and the state agree on.

| State | Means |
|---|---|
| **received** | the hub has it; nothing decided yet |
| **refused** | a level, a guard or a limit said no; never sent to the provider |
| **asking** | waiting for the user's answer (a question is up) |
| **denied** | the user said no, or the question closed unanswered; never sent |
| **dispatched** | sent to the provider; its effect may be happening |
| **completed** | the provider answered with a result (an error result counts: the provider ran it) |
| **cancellation requested** | cancelled after dispatch (MCP's cancellation sent); the provider may or may not have stopped |
| **outcome unknown** | dispatched, and no answer will come: the hub restarted, `CallDeadline` passed, or the provider went away |

- **Only refused, denied and calls cancelled before dispatch** are "nothing
  was done". A call that was dispatched ends completed, or outcome unknown,
  never "failed, try again".
- **Outcome unknown reaches the agent** as a tool error that says so: "this
  may or may not have taken effect; check before doing it again". The hub
  never retries a call, and never tells an agent to.
- **A cancellation is a request:** MCP's cancellation asks the provider to
  stop; it may finish anyway. The call stays listed (`Active`, marked
  cancellation requested) until the provider answers or its deadline
  passes, then ends completed or outcome unknown. A result arriving after
  the agent was told is recorded in the audit, not sent.
- **Deadlines:** `CallTimeout` ends a call nothing is heard of; each
  progress notification restarts it, but never past `CallDeadline`, an
  absolute limit from dispatch that progress doesn't extend. At most
  `MaxInFlight` calls run at once per agent and per provider; more wait.
  (Long-running work belongs to MCP's tasks, once the hub supports them.)
- **High-impact calls are recorded before they're sent** (`exec`,
  `destroy`, `capture`, `input`, and `Reach=outside` tools): an intent line
  with the call's id, synced to disk, then the dispatch, then a line with
  the same id when it ends. A hub starting after a crash marks each intent
  with no ending as outcome unknown.

## State for surfaces

The hub publishes its state on D-Bus, at `org.freedesktop.AgentHub1` on
the session bus (`doc/dbus/org.waybox.AgentHub1.xml` until renamed; see
"Names").

**Properties:**
- `Bindings`: agents bound to profiles: id, profile, label, the token
  file's path (never the token), remote, an app id for a sandboxed one.
- `Unconfirmed`: values the settings file raises that the user hasn't
  confirmed: key, the file's value, the confirmed value in force.
- `Agents`: connected agents, with profile, self-declared name, the user's
  label, whether remote, the app id for a sandboxed one, and which
  sensitivities it has had data from (the flow guard's record).
- `Pending`: questions waiting for the user's answer.
- `Providers`: id, name, state (`approved`, `pending`, `refused`), tool count.
- `Grants`: what's granted, to which agent connection (and its profile),
  until when; each goes when its connection closes.
- `Active`: what's happening now, such as capture or input in progress, or
  a tool call running.

**Signals:**
- `CallStarted(call)`, `CallEnded(call)`: who called what, and the
  outcome as a fixed code with a short explanation the hub writes itself;
  never the arguments, and never a provider's own error text (which may
  echo them): that goes to the agent, and to the audit, redacted.
- Property changes come as D-Bus's standard `PropertiesChanged`, so there
  are no signals of its own for providers or grants changing.

**Methods:**
- `Revoke(grant)`
- `RevokeAll()`
- `Stop()`: what the stop key does, from a panel button: the hub asks the
  compositor (`StopAssistants`), which does exactly what the key does and
  sends `Stopped`; the hub then does its part as for the key. Calling it
  again changes nothing.
- `RefuseProvider(id)`
- `RecentCalls(count)`: the latest calls, metadata only, with `count`
  capped (`RecentCallsMax`)
- `Propose(change)`: ask the compositor to show the confirmation for a
  change that raises power (approve a provider, bind an agent, raise a
  level, resume after a stop). It only asks: the user's physical answer
  decides, so calling it grants nothing. Rate-limited (`ProposeRate`) and
  logged.

**Who may call:**
- **Methods:** callable by any process of the user (the session bus already
  limits this to the user).
- **Nothing that grants.** Granting, approving providers and binding
  profiles happen only through the compositor's confirmations. A panel can
  show and take away, never give.
- **Fail-safe abuse only:** a program spamming `Stop` or `RevokeAll` can only
  make things safer; one spamming `Propose` can only show the user
  questions, up to the rate limit, each of which defaults to no. Such calls are rate-limited and logged.

**No arguments on D-Bus.** Properties, signals and `RecentCalls` carry who
called what, when, and the outcome, never the arguments. The session bus
reaches more programs than should see what an agent typed or read, including
sandboxed apps with bus access. Arguments are only in the audit file.

**Trust the compositor's indicator.** Any program can claim the hub's bus
name and show a fake "idle". The compositor's own indicator is the one that
can't be faked; a panel's is a convenience.

A panel module would show an indicator while `Watching` is true, list
grants, and offer revoke and stop. That needs nothing more than the
properties and signals above.

**Waybar needs no change to be a surface.** A custom module runs a small
script that follows the hub and prints Waybar's JSON. Click is `Stop`,
right-click is `RevokeAll`.

**Short calls are too brief to show.** Most calls start and end within
milliseconds, so `Active` flickers: an indicator follows `Watching`
(capture or input) and grants, not `Active`.

**Pending providers deserve a state of their own** on a surface (a new
program wants to offer tools), so the user notices and decides.

## The audit log

The hub appends one JSON line per event to
`$XDG_STATE_HOME/agents/tools/audit.log`:

```json
{"v":1,"time":"2026-11-28T10:01:02.123Z","event":"call","call":"c-4f1a",
 "agent":{"conn":17,"profile":"trusted","claim":"Claude Code","label":"laptop",
          "binding":"sha256:3f2a…","remote":false},
 "provider":"org.waybar.Waybar","tool":"media_play_pause","name":"waybar__media_play_pause",
 "sensitivity":"act","args":{},"decision":"asked-allowed","outcome":"completed","scope":"session",
 "why":"level","ms":12,"result":{"bytes":31}}
```

- **`event`:** `call`, and the others the hub does: `connect`,
  `disconnect`, `question`, `confirm`, `grant`, `revoke`, `stop`,
  `provider` (pending, approved, refused, changed), `settings` (raised,
  lowered), `refused-request` (sampling or elicitation), `hub` (started,
  restarted).
- **`decision`** (calls): `allowed`, `asked-allowed`, `asked-denied`,
  `refused`; **`outcome`**: the state the call ended in (see "Calls"):
  `completed`, `refused`, `denied`, `cancelled` (before dispatch),
  `cancellation-requested`, `outcome-unknown`. A high-impact call has an
  `intent` line before it's sent, and its ending line has the same `call`
  id.
- **`why`:** what decided it: `level`, `grant`, `flow-guard`, `policy`,
  `locked`.
- **Arguments** with secrets redacted (each tool's `Secret`); a string
  longer than `AuditCut` is cut, marked so (`"cut": true` and its full
  length).
- **Kept in full where the spec promises it:** `run_lua`'s code and
  `run_command`'s command line go into a file of their own beside the
  audit (`audit.d/<call id>`, mode 0600, at most `AuditKeepMax` each), its
  SHA-256 in the line; rotated with the audit. One larger than that is
  kept cut, and the line says the record is incomplete.
- **For `capture`:** what was captured (window or output, size). **For
  `input`:** the target window, the kind of input (text, keys, clicks) and
  how much (characters, keys), never the text typed: the compositor can't
  tell a password field in another application from any other, so typed
  text isn't recorded at all by default. `AuditInput=full` records it, a
  setting the settings show with a plain warning that passwords typed for
  an agent then land in the file.
- **Rotation:** past a size (`AuditRotate`) the file becomes
  `audit.log.1`, a few kept.
  Unknown fields are ignored by readers; `v` changes only for a change that
  breaks them.

Surfaces can follow it through `CallEnded`, which carries no arguments.

**The file is the user's alone:** mode 0600, in a directory with mode 0700.
It's on by default. Turning it off counts as raising power, so it needs the
compositor's confirmation.

## The compositor's part

**No Wayland protocol is involved or defined.** Agents, shims, the hub,
providers and surfaces aren't Wayland clients. The compositor and the hub
talk over the two sockets the compositor hands the hub at launch: the
compositor interface, and the compositor's tools as MCP.

### The compositor interface: `org.freedesktop.AgentCompositor1`

Peer-to-peer D-Bus (no bus daemon, no bus name) on the inherited socket,
object `/org/freedesktop/AgentCompositor1`. Everything in it takes and
returns `a{sv}` for what may grow; unknown keys are ignored. The sketch:

- **`Hello(u version, a{sv} hub) → (u version, a{sv} compositor)`:** each
  side's version and what it supports (`capture`, `input`, `indicator`,
  the stop key's binding); the hub offers only what the compositor said it
  supports.
- **`Ask(a{sv} question) → u id`**, answered later by
  **`Answered(u id, s answer, a{sv} details)`**: a question about a call.
  The question carries the profile, the agent's claimed name, the
  provider's name, the tool, its sensitivity, the arguments' summary, and
  the scopes to offer. `answer` is `deny`, `once`, `session` or `timed`
  (with `until` in the details). **`CloseQuestion(u id)`** withdraws one;
  it counts as denied.
- **`Confirm(a{sv} change) → u id`**, answered by **`Confirmed(u id, b yes,
  a{sv} details)`**: a change that raises power (a level, a guard, the
  audit, approving a provider, binding a profile), with what it is, shown
  in full.
- **`Capture(a{sv} target, a{sv} options) → h image`:** an image of a
  window or an output, drawn by the compositor (private windows and the
  lock screen left out), as a file descriptor (a sealed memfd, PNG).
- **`OpenInput(a{sv} target) → h eis`:** a connection in libei's protocol
  (EIS), limited by the compositor to the target window; closed by the
  compositor at the stop key, when the window goes, or when the screen
  locks.
- **`SetActivity(a{sv})`:** what the hub is doing, for the compositor's
  indicator (capture and input it already knows; a call running).
- **`Stopped()`:** the stop key was pressed. The compositor has already
  ended capture and input and closed its questions; the hub revokes grants
  and cancels calls.

**Windows are named by their `ext-foreign-toplevel-list` identifier,**
the one Wayland itself defines across compositors, so a window means the
same thing to the hub, to tools and in the audit on any compositor.

**Why not the ScreenCast and RemoteDesktop portals for capture and input:**
the hub would call them, and they'd ask their own question on top of the
hub's; RemoteDesktop grants input to the whole desktop, where the hub
grants one window. libei, which RemoteDesktop uses underneath, is kept: it
is the standard for synthetic input, so a compositor that has EIS for the
portal has most of `OpenInput` already.

**A compositor implementing this provides:**
- **the compositor interface,** answered only on the hub's socket:
  capture and input for the hub's own tools, the stopped and away states;
- **its tools,** as an MCP provider with a manifest (windows, desktops:
  what's its own);
- **confirmations and questions,** drawn by itself, answered by physical
  input only;
- **the stop key,** always, and the stopped state it owns;
- **the agent's processes** held to their levels (`AgentProcess`), and UI
  providers' windows kept from taking focus (`UiRequest`);
- **the indicator,** with its states (see "The indicator"). Switchable; a
  surface may show one too.

### A compositor's own tools: Waybox's

What a compositor offers as its own provider is its own choice; Waybox's,
as an example:

| Tool | Sensitivity | What it does |
|---|---|---|
| `list_windows` | read | windows without their titles: id, app id, desktop, output, geometry, state, focused; each window's process id and whether it answers (Waybox's ping) |
| `window_titles` | content | each window's id and title |
| `list_desktops`, `list_outputs`, `get_version` | read | desktops and their names; monitors, modes, scale; Waybox's version |
| `window_process` | read | a window's process: its state, CPU use, what it's waiting on, its children (the stuck-window case) |
| `check_config` | read | Waybox's parser on a proposed rc.xml, without applying it |
| `focus_window`, `move_window`, `resize_window`, `set_window_state`, `send_to_desktop`, `switch_desktop` | act | as their actions do |
| `close_window` | destroy | asks the window to close, as its close button |
| `kill_window` | destroy | ends the process that owns a window (Waybox's Kill), for one that doesn't answer: every window of that process goes, with any unsaved work; the question names them. A first call asks the process to end (`SIGTERM`); calling it again within a few seconds forces it (`SIGKILL`), which always asks afresh, whatever grant covered the first |
| `run_action` | exec | any rc.xml action by name (Execute among them) |
| `run_command` | exec | a command, as Execute runs it |
| `write_config` | exec | rc.xml replaced (backed up, checked first, then Reconfigure): it can run commands |
| `run_lua` | exec | Lua run in Waybox (when built with Lua), the code kept in full beside the audit (see "The audit log") |

### The hub's own tools: capture and input

Capture and input are the same on every desktop, so they're the hub's own
tools (the reserved `desktop` provider), built on the compositor
interface, not the compositor's provider's:

| Tool | Sensitivity | What it does |
|---|---|---|
| `desktop__capture_window`, `desktop__capture_output` | capture | an image of a window or a monitor, drawn by the compositor |
| `desktop__type_text`, `desktop__press_keys`, `desktop__click` | input | synthetic input into the granted window |

One owner for each step:
- **The hub** checks the arguments against the tool's schema, decides
  (levels, guards, consent), records the intent, and calls the compositor:
  `Capture(target, options)` or `OpenInput(target)`.
- **The compositor** draws the capture (private windows and the lock
  screen left out) into a sealed memfd, or hands out an EIS connection
  limited to the target window, and owns ending it: at the stop key, when
  the window goes, when the screen locks.
- **The hub** turns a capture into MCP image content (base64 PNG, within
  `MaxResult`; `max_size` keeps it so) and closes the memfd; for input,
  it holds the EIS connection, sends the events through libei, and closes
  it when the call ends, is cancelled, or the grant goes. An EIS
  connection the compositor closed ends the call as the stop key does.
- **The audit** records the call as any other, with what was captured and
  the input's kind and size (see "The audit log").


**Not offered: raw IPC.** It would be every typed tool without the typing,
and the audit would only see "raw".

**The accessibility tree** comes from a provider of its own, `atspi`
(AT-SPI, the accessibility bus): `content`, `off` in "safe".

## Scripts as providers

Waybox's Lua scripts offer tools with `wb.mcp_tool(...)`, each declaring
its sensitivity, reach and schema in the script.
- **Each script's tools are a provider of their own** (`Id`
  `<compositor>.script.<script name>`), approved like any other: the
  compositor builds its manifest from the script's declarations, and the
  approval pins a digest of that manifest and of the script's file. A
  script whose tools or file changed is pending again; its tools are
  offered to no agent until the user approves what they now are.
- **A policy hook,** `wb.policy("mcp", fn)`, sees every call routed through
  the compositor (its own tools and scripts' tools) after the hub has
  allowed it, and can refuse it; it can't allow what the hub refused.

## Agents in practice

**Most agents are host programs with a shell.** Distributions package them
as ordinary programs (Fedora: `pi-coding-agent`, `openclaw`,
`hermes-agent`, Emacs's `agent-shell`), often with a local model
(`ollama`, `llama-cpp`). For these, a binding with a token file is the way
in, and the levels confine them only as far as they're otherwise confined
(see "The limits"). The Agent portal is for sandboxed agents, and for
whoever wants an agent truly confined.

**Recommended:** the agent sandboxed wherever the work allows it, or at
least its own command tool off (`ShellOff=` in its integration file, which
the settings offer when adding it). Turning the command tool off closes one
way around the hub, not all: the agent may still write files that
something later runs, use other MCP servers, reach the network, or run a
skill's scripts. Only a sandbox makes the hub its only way out.

**Their models may be anywhere:** on the machine, on the network (a model
server on the LAN), or a cloud service. Only the agent's client talks to
the hub, so where the model runs doesn't change what the agent may do.

### Agent integrations

Each agent keeps its MCP servers in its own place and format: a JSON file
here, a command there (`openclaw mcp add`, `claude mcp add`). So adding the
hub to an agent is described by an **agent integration file**, which the
settings use for "Add assistant" (bind it, then write its configuration).
It's named by the agent's app id, its desktop file's (`<app id>.ini`), so
an integration file and the app it describes can't be confused, and no
registry of names is needed:

```ini
# $XDG_DATA_DIRS/agents/clients/org.example.OpenClaw.ini
[Agent]
Version=1
Name=OpenClaw
Binary=openclaw
# Either a command that adds a server (%s: the shim's command, as words)…
McpAdd=openclaw mcp add desktop -- %s
McpRemove=openclaw mcp remove desktop
# …or a JSON file and the object holding its servers:
# McpConfig=~/.pi/agent/mcp.json
# McpKey=mcpServers
# How to turn off the agent's own command tool, where it has a way
# (a command, as McpAdd), offered by the settings when adding it:
# ShellOff=…
# How to start it with a question, for "Ask AI about this window": a
# command, as McpAdd (%q: the question, as one word), or a URL, opened
# as the desktop opens links (%u: the question, percent-encoded)
Ask=foot -e openclaw ask %q
# Where it's one app serving every conversation, running already or
# started once (a desktop app): its tools connection is shared
AskShared=false
```

- **Who ships them:** the agent's own package; the user's own go in
  `$XDG_DATA_HOME/agents/clients/`.
- **The agent's own description is the authority.** Until an agent ships
  one, a distribution or desktop may describe it in the fallback folder
  (`agents/clients/fallback/`), read only for app ids described nowhere
  else: once the agent ships its own, it's used instead, no package owns
  the same path twice, and a fallback can't override what the agent says.
- **Which is used, for an app id:** the user's file; the agent's own
  (a file from its package, or its desktop file's group, below); a
  fallback file.
- **The shim's command** (`%s`) is the desktop's (`waybox-mcp --token-file
  …`), so one file serves every desktop.
- **A JSON file is edited in place,** the rest of it left as it was, and
  backed up first; a command is run without a shell.
- **Unknown agents** still work: the settings show the command to add by
  hand.
- **A desktop app** is described the same way: its `McpConfig=` is its own
  config file, and its `Ask=` a URL it handles, such as
  `claude://claude.ai/new?q=%u` with `AskShared=true`.

**In the agent's desktop file:** an agent may carry its description in its
own desktop file instead, as the Desktop Entry spec's interfaces have it:
`Implements=org.freedesktop.AgentClient1` in its `[Desktop Entry]`, and the
same keys in a group named after the interface. That's how a sandboxed
agent (Flatpak, Snap) announces itself: it can't install into
`/usr/share/agents/`, but its desktop file is exported, and Flatpak exports
a group named in `Implements=` as it is.

```ini
# org.example.Assistant.desktop
[Desktop Entry]
Type=Application
Name=Example Assistant
Exec=example-assistant
Implements=org.freedesktop.AgentClient1

[org.freedesktop.AgentClient1]
Version=1
Ask=example-assistant://new?q=%u
AskShared=true
```

- **A sandboxed agent connects through the Agent portal,** not the shim, so
  it needs no `McpAdd=` or `McpConfig=`: the settings show it as an
  assistant that asks to connect.
- **Its commands run in its sandbox:** the desktop runs a command from
  the group as the app (for Flatpak, `flatpak run --command=…` with its app
  id), never on the host; a URL is opened as the desktop opens links.

### Asking about a window

The window menu (Alt+Space, or the titlebar's) offers "Ask AI about this
window" when an assistant is set up for it: `AskWith=` lists the asks, in
order, each a binding (with its agent's `Ask=`) or an `[Ask …]` of the
user's own (a binding with its own `Ask=`, for one model or another). One
ask: the entry itself; several: a submenu of their labels, the user's
names for them, never what agents call themselves. The hub tells the
compositor what to show (`SetOffers`, `asks`).
- **Only the click chooses.** No window, app or agent picks the
  assistant, and nothing is sent until the user chooses one.
- **The click is the consent,** for that one window and that one ask:
  it's physical input in a menu the compositor draws, which is what the
  spec trusts. The compositor tells the hub (`AskAbout`, with the
  window's id and the ask's).
- **The hub starts it** with its `Ask=`: a command, or a URL opened as the
  desktop opens links. The question names the window by its id and app id
  only, never its title: titles are untrusted (a page titled "ignore your
  instructions…"), so the assistant reads it through the tools, marked
  untrusted like any other. Where the agent can, the question is prefilled
  for the user to send, not sent for them.
- **The grant goes to what the click started:**
  - **An assistant started for the question:** the hub puts a one-time
    ticket (128 random bits) in its environment (`AGENT_TOOLS_ASK`); its
    shim presents it in the handshake (`ask`). The ticket works once,
    within `AskTicket`. That connection gets the grant, while it lasts.
  - **A shared one** (`AskShared=true`: one app serving every
    conversation, its tools connection made already, its environment not
    the hub's to set): the grant goes to that binding's connections, for
    `AskGrant` or until the window closes, whichever is first. The app's
    other conversations share it; the settings say so beside such an ask.
- **What it grants:** `content` and `capture` for that window (its title,
  its accessibility tree, a capture of it); nothing else is raised (Kill
  and the rest follow the profile, at `ask` where they are). Recorded in
  the audit as a grant made by the menu, with the ask's name.

### Unattended agents

Some agents act on their own: on a schedule, or "proactively" (OpenClaw's
word), while nobody is at the desk.
- **While the user is away** (idle past `AwayAfter`, or the screen locked;
  the compositor's `Away`), questions aren't shown: there's no one to answer, and a waiting
  call would outlast its client's timeout. The call is denied at once with
  a tool error the model understands ("the user is away; nothing was
  done").
- **A question already up when the user goes away** closes at once, denied
  ("the user went away"), leaves `Pending`, and joins the list below; it's
  never shown again, since its call may no longer be timely.
- **On return,** a notice lists what was asked meanwhile (agent, tool,
  arguments' summary, when), with "Review". A denied call is never
  replayed or granted after the fact. From Review, for each entry: an
  agent still connected can be given a grant for its next calls, and any
  agent a level change for that tool in its profile; both through a
  confirmation the compositor draws, as any raise. An agent that has left
  gets no grant: its next run asks as usual.
- **A profile can set levels for while the user is away** (`Away=read`:
  read and content only), so an assistant can keep watching without acting.

### Clients' timeouts

Agents give up on a call after their own timeout: from seconds to a day,
depending on the client and its settings. A question can outlast it.
- **While a question waits,** the hub sends MCP progress notifications for
  the call (every `ProgressInterval`), where the agent gave the call a
  `progressToken`; clients that count progress as activity keep waiting.
  Without a token there's nothing to send, and the client's own timeout
  decides.
- **A client that cancels anyway** closes the question (MCP's
  cancellation), and the call counts as cancelled, not denied.

### Agents with a frontend of their own (ACP)

Many agents are driven by a frontend (an editor, a chat window) through the
Agent Client Protocol, which has the agent ask that frontend before running
a tool (`session/request_permission`: allow or reject, once or always).
- **They answer different questions.** ACP's is the agent asking its user
  whether to try a tool; this spec's is the desktop deciding whether the
  agent may act on it. Only the compositor's answer authorizes a desktop
  action: a frontend's "allow always" grants nothing here.
- **Asked twice:** a desktop tool may be asked about by both. A frontend
  can avoid that by trusting the hints: tools at `ask` are marked by the
  hub (`_meta` `org.freedesktop.agent/asks`), so a frontend may skip its
  own question for them, the compositor asking anyway.

### The desktop's skill, and skills that act

**A skill ships with the hub** (an Agent Skills `SKILL.md`, in
`/usr/share/agents/skills/desktop/`), teaching any agent how to work here:
list before acting; a denial means stop, not retry; what "stopped" and
"the user is away" mean; why the flow guard asks. Agents load it when it's
relevant, so the tools' own descriptions stay short. It says how to work,
never what's allowed: levels and grants come only from the hub.

**Skills that change things on the desktop** should do it through the
hub's tools, not their own scripts: the change is then asked about and
audited. A skill's scripts run with the agent's rights, outside the hub,
as an agent's shell does (see "The limits"). Consent written into a skill
as instructions (a checklist the model is told to follow) is a request to
the model; the hub's is enforced.

## The agent's processes

A prompt-injected agent with a shell doesn't need the hub to take a
screenshot (`grim`), type (`wtype`) or close a window (the compositor's own
IPC): it runs a program that does. Those programs are Wayland or IPC
clients of the compositor, so the compositor can tell that they act for
the agent, and hold them to its levels.

- **The hub tells the compositor the agent's process:** at the handshake
  (from the shim's parent), `AgentProcess(conn, info)`: a pidfd where the
  system has them, otherwise the process id with its start time (so a
  reused id is told apart); `ForgetAgent(conn)` when it leaves.
- **On Linux, the agent's processes are kept together in a scope,** which
  the compositor reads from a client's process. Moving a running process
  into a scope doesn't move the children it already has: only what it
  starts afterwards follows it. So:
  - **Started in its scope (tracked from the start):** the agent is
    launched inside a scope of its own from the beginning, by its
    integration (`agents-run LABEL -- COMMAND`, which asks the user's
    service manager for the scope, then starts the agent in it: what
    desktop entries and the settings' snippets use). The hub checks at
    the handshake that the agent is in such a scope.
  - **Already running (tracked from connecting):** the hub moves the agent
    into a new scope (systemd's `StartTransientUnit` with its process);
    helpers it started before are outside it. Best effort, and said so:
    the agent is listed (`Agents`, the indicator's details) as "tracked
    from connecting".
  - **No scope possible** (no user service manager): the parent chain,
    best effort, listed so.
- **Elsewhere,** the parent chain (weaker: a process whose parent exits is
  adopted by another), listed as best effort.
- **What the compositor holds to the agent's levels,** for clients in its
  scope: screen capture protocols (`capture`), virtual keyboards and
  pointers (`input`), clipboard and window management protocols (`content`,
  `act`), and requests on its IPC socket (by what they do: `read`, `act`,
  `destroy`, `exec`). Allowed ones go through; others ask, the question
  saying a program the agent started wants it; both go in the audit (the
  compositor tells the hub, `ClientCall`).
- **The limits:** work started through the user's service manager
  (`systemd-run --user`) leaves the scope; signals, files and the network
  aren't the compositor's. This catches a tricked agent's ordinary tools,
  not a determined escape: for that, confine the agent.

## The Agent portal: sandboxed agents and providers

An agent with a shell can do anything the user can (see "The limits"). An
agent in a sandbox whose only door is this portal is confined: the levels
are then all it can do. So the portal is the recommended way to run an
agent with real limits, not only the way for sandboxed apps.

**`org.freedesktop.portal.Agent`**, after the ScreenCast portal's pattern:
- **`CreateSession(a{sv} options) → o handle`**
- **`Start(o session, s parent_window, a{sv} options) → o handle`:** the
  user's answer, asked by the compositor: "*App* wants to use your
  desktop's tools as an assistant", the profile it would get, and whether
  to remember. Remembered answers return a `restore_token`, as screen
  sharing's do, and are kept in the permission store's `agent` table,
  listed and revoked in the settings with the profiles.
- **`ConnectAgent(o session, a{sv} options) → h socket`:** a socket that
  speaks MCP to the hub, as a shim's does. The portal doesn't parse MCP;
  it connects and steps aside.
- **`RegisterProvider(o session, a{sv} options) → h socket`:** the same for
  a sandboxed provider (or an Agent UI provider), its manifest's `Id` its
  app id.
- **A session has one role,** fixed by its first `Start` (`agent` or
  `provider`): starting it again, or asking for the other socket, is
  refused. Remembered answers and restore tokens are per app id *and*
  role: an approval to use tools never lets an app offer them, nor the
  reverse (and a provider still needs its manifest approved).
- **Sockets end with their session:** each socket the portal hands out
  belongs to its session, and the hub closes it (cancelling its calls, as
  for any agent leaving) when the session is closed, when the caller's bus
  connection goes, when the user revokes the app's remembered answer, or,
  for a provider, when it's refused. A sandboxed app holds the socket only
  as long as the portal would answer it.

**Identity:** the portal knows a sandboxed app's id for certain (Flatpak
and Snap both make it known, and every portal relies on it), and passes it
to the hub. A sandboxed agent's profile is bound to its app id: no token
to copy.

**The backend is the hub** (`org.freedesktop.impl.portal.Agent`): the
portal frontend checks the caller and hands the hub the app id; the
compositor asks the question, through the compositor interface, as for
any other.
- **Only the portal frontend may call the backend:** the hub accepts a
  backend call only from the bus connection that owns
  `org.freedesktop.portal.Desktop` (xdg-desktop-portal, a host process),
  checked on each call; the `app_id` it passes is trusted because of who
  passes it, never because it was passed.
- **Separate connections:** the backend, the hub's state (`AgentHub1`) and
  the interim frontend (below) each sit on a bus connection of their own.
  Bus permissions are per connection: a sandbox allowed to talk to one
  name may reach every name and object its owner's connection has.

**Manifests from inside a sandbox:** a sandboxed provider carries its
manifest in its desktop file, as a sandboxed agent does its integration
(`Implements=org.freedesktop.AgentProvider1`, and the manifest's keys in a
group of that name), which Flatpak exports as it is. Its `Id` is its app
id, and checked against it.

**No static holes:** no `--talk-name`, no socket mounted into the sandbox.
Portals are reachable from any sandbox, and the user's answer is the
permission. (The interim bus name, under "Names", is the one exception
until the portal frontend exists: one `--talk-name`.)

**How the caller's app id is known,** by the portal frontend, and by the
hub itself for the interim bus name, the same way xdg-desktop-portal does:
- **From the D-Bus sender, never from what the caller says:** the bus's
  credentials for the sender give its process as a pidfd (not a PID,
  which can be reused), so the process can't change between the check and
  its use.
- **Flatpak:** the process's `/.flatpak-info` (through its root, as the
  portal reads it), which the app can't write.
- **Snap:** the process's security label from the bus's credentials
  (`snap.<name>.…`).
- **Not sandboxed:** refused; a host agent uses the shim.
- **A live session is bound to its sender:** the session handle belongs to
  the verified app id and to the sender's unique bus name, which names one
  bus connection; a call on it from any other sender is refused and logged.
- **A restore token is bound to the app, not to a connection:** to the
  verified app id and its remembered answer (the `agent` permission record),
  since the app's unique name changes whenever it reconnects. Redeeming
  one, the hub verifies the new sender's app id the same way, checks that
  the record still allows it (not revoked in the settings), and issues a
  new session handle bound to that sender. A token presented by another
  app id is refused and logged.
- A provider's manifest `Id` must be that app id.

If the hub can't establish this for the interim name on a system (no
pidfds in the bus's credentials, say), it offers no sandboxed agents there
until the portal frontend does.

## Agent UI

The other direction: an agent, or a plain script, that talks to the user
through whatever UI the desktop has, not one of its own. It asks a question,
offers a choice, fills a form, shows a notice, streams output, asks for a
file.

### Tools

The hub offers them to agents like any provider's tools, as the `ui`
provider, with sensitivity `ui`:

| Tool | What the user sees | Returns |
|---|---|---|
| `ui__notify(title, body, actions?)` | a notice, with buttons if given | the button chosen, or nothing |
| `ui__ask(prompt, default?, secret?)` | a one-line question | the text, or cancelled |
| `ui__choose(prompt, items, multiple?)` | a list to pick from | the item(s), or cancelled |
| `ui__form(title, schema)` | a form | the values, or cancelled |
| `ui__output_open(title)`, `ui__output_append(id, text)`, `ui__output_close(id)` | a stream of output (a log, a long answer) | an id |
| `ui__progress(id, fraction?, text)` | progress on a running task | nothing |
| `ui__pick_file(prompt, save?, filters?)` | the file chooser | the path, or cancelled |

`ui__form` takes **MCP elicitation's schema**: a flat object of text,
number, boolean and choice fields. MCP restricted it to what any UI can
render, and reusing it keeps one form language across the agent world.

A **flow** groups requests: each tool takes an optional `flow` id. A provider
may then show consecutive requests of one flow as one window (a wizard)
rather than as separate pop-ups.

### UI providers

Whatever is running renders the requests; the agent never knows what drew
anything. A **UI provider** is a desktop component that implements
`org.waybox.AgentUI1` (`doc/dbus/org.waybox.AgentUI1.xml`) and declares in
its manifest which kinds it renders. It registers with the hub as any
provider does (its executable checked, or through the Agent portal), and
gets its requests over that registration's private socket, as
peer-to-peer D-Bus, never on the session bus by a well-known name: a
program that took a name first could otherwise receive the questions and
the user's answers. Its manifest:

```ini
[UI]
Kinds=choose,ask
```

The user picks a provider per kind (`ask`, `choose`, `form`, `output`,
`progress`), with fallbacks. Two kinds are never a UI provider's, since
standards cover them and one carries the user's consent to share a file;
the hub uses those directly:
- **notify:** freedesktop Notifications, with its actions;
- **pick_file:** the portal FileChooser. It's sandbox-aware, and the user's
  pick is itself the consent to share the file. The chooser grants access
  to its caller, the hub, so what reaches the agent depends on it:
  - **A host agent** gets the path.
  - **A sandboxed agent** (through the Agent portal) gets the file through
    the document portal, for its app id only, and the path the app sees
    inside its sandbox (under `/run/user/…/doc/`). Another app can't use
    it.
    - **Opening:** the chosen, existing file is added with `AddFull`,
      granting read.
    - **Saving:** the chosen folder and name with `AddNamedFull`, which
      works for a file that doesn't exist yet, granting read and write to
      that one file, nothing else in the folder. Replacing an existing file
      is what the chooser already asked the user about.
  - **Access ends** when that agent's connection closes, the hub revoking
    the app's permissions on the document, unless the user chose to keep
    it in the chooser.

**Waybox ships fallbacks for every kind,** so all of it works with nothing
else installed:
- **choose:** its menu;
- **notify:** its on-screen display;
- **ask, form, output:** a small built-in window.

A launcher (rofi, fuzzel), the notification daemon, a panel or a chat app
can take over the kinds they do better.

**The hub's part:**
- **Routing:** requests go to the provider the user chose for their kind.
- **Providers always answer:** with an error if they fail. One that doesn't
  answer within `UiTimeout` is treated as failed, and the hub then uses the
  next provider for that kind (Waybox's built-in ones last); the agent gets
  that one's answer, or a plain error if none answers, never a hang.
- **One live prompt per request:** each request has an id of the hub's.
  Before falling back, the hub closes the request on the first provider
  (`Close`) and waits briefly for it to confirm; an answer arriving later
  is discarded and logged. A provider that doesn't confirm closing means
  no fallback for a secret field or a form: the request fails visibly
  instead of a second prompt collecting the same answer.
- **Order:** a queue per agent, and timeouts.
- **Ending:** an agent's open requests close when it disconnects, or at the
  stop key, or when the user mutes it.

**Works without an agent:** a headless script can use the same tools,
through a shim, to pop a form and read the answer.

### Agent UI is untrusted UI

Everything an agent shows is controlled by the agent, and so by whatever has
injected instructions into it. The rules:

- **It always looks like agent UI.**
  - **Labelled:** "from an assistant calling itself …", the same claim
    wording as consent.
  - **Distinct:** its own frame, never the style of the compositor's
    confirmations or of a system password prompt (polkit).
  - **Kept apart:** a UI provider must not render the compositor's
    confirmations, and the compositor never renders agent UI in the
    trusted style. The line between trusted and agent UI never blurs.
- **Secret fields warn.** A `secret` question, or a form field that asks
  for a password, says plainly that the answer goes to the assistant, not
  to the system.
- **It never takes focus from what the user is doing.** A request appears
  as a notice or a waiting entry the user chooses to open. Otherwise it
  would catch the keystrokes of someone typing elsewhere.
  - **The compositor enforces it.** A provider can't keep the compositor
    from focusing its new window, and a question that opens focused, its
    text selected, catches whatever the user types next. So the compositor
    decides by who opened it, not by
    what the window says: the hub tells it a UI provider's process while
    that provider has a request open (`UiRequest(conn, info)` on the
    compositor interface, as `AgentProcess`), and any window that process
    opens meanwhile is agent UI: never focused when it appears, opened
    beside, not over, the focused window, and framed as agent UI. The
    `agent-ui` tag (xdg-toplevel-tag) is a description only. A provider
    whose windows can't be told apart from its other ones (a launcher the
    user also uses itself) gets requests drawn by the compositor's own
    fallback instead.
  - **Where a provider opens a window,** input is ignored briefly after it
    appears, as with consent.
- **Keyboard:** Enter submits (once the brief pause is over) and Escape
  cancels, as in any dialog.
- **Rate limits per agent,** and the user can mute an agent's UI. The stop
  key closes all of it.
- **The audit records who asked what:** the agent, the kind, the prompt, and
  whether it was answered.
  - **Answers aren't logged:** they're the user's input to that agent, given
    knowingly, and may be private.
  - **A secret question's prompt isn't secret** and is logged: redacting it
    would hide what a phishing attempt asked.
  - **Files:** a file picked in the chooser goes to the agent because the
    user picked it.

## Threat model and security

### Who the design defends against

| Attacker | Example | Defended by |
|---|---|---|
| **A prompt-injected agent** (the main threat) | a web page or window title telling the model to run commands | levels, consent, grants scoped to the agent, the flow guard, the stop key, the audit; for programs it starts, the compositor holding them to its levels (best effort, see "The agent's processes") |
| **A provider tampering with its tools' definitions** | a tool that changes its description or schema to steer the model | the pinned definition (manifest and schemas), reserved names settled at approval |
| **A malicious or compromised provider's code** | an approved provider, or an update of it, reading files or sending data out | nothing in this design: approving a provider trusts its code, as installing it does; its sensitivities and `Reach` are its promise, and the audit shows what it was called for |
| **Another program of the user** | an app trying to grant itself access | nothing it can do *through this design* gives it more than it had; see below |
| **A sandboxed app** | a Flatpak with session bus access | no arguments on D-Bus, nothing that grants over D-Bus, no access to the compositor's private connection, the interim portal alone on its bus connection, its app id taken from the bus, never from what it says |
| **An agent phishing the user** | a form asking for a password, styled like the system's | agent UI always looks like agent UI, secret fields warn, no focus stealing |

### The limits

**An agent that has a shell is a program of the user.** Many agents (coding
agents among them) can run commands outside the hub. Such an agent can do
anything the user can, except what needs the compositor's confirmations: it
can't raise levels, approve providers, bind profiles or answer questions,
because those take physical input in a dialog the compositor draws.

What this design gives, said plainly:
- **An unconfined agent** (its own shell on): consent and an audit for the
  tools it uses through the hub, and a net for its ordinary way around
  them: screenshots, typing and window actions by programs it starts are
  caught by the compositor (see "The agent's processes"). Not caught: an
  agent set on escaping (starting work through the user's service manager),
  signals (`kill`), files and the network.
- **A confined agent:** one in a sandbox with no way out but the hub (no
  host files beyond what it's given, no network unless through a
  provider, no other IPC, no running host programs): the levels are
  everything it can do. Only that is called confined.
- **An agent with its command tool off** isn't confined: one way around the
  hub is closed, others remain (writing files something later runs, other
  MCP servers, the network, a skill's scripts), unless its configuration
  closes those too. The settings say "command tool off", never
  "confined", for it.

For real confinement, run the agent in a sandbox where the hub's socket
(or the Agent portal) is the only way out.

**Data can flow between providers through the agent.** Content read from
one tool (an accessibility tree, notification text) can be passed as
arguments to another provider's tool, which might send it off the machine.
Levels per provider (keeping a networked provider at `ask`) and the audit
are the defence. Rules about where data may flow are out of scope for now.

**Read results can carry injections.** Window titles and other text from
applications reach the model, and are allowed even in "safe". Results
are marked as untrusted content, for agents that understand the marking;
for those that don't, the hub's instructions say so.

### Rules that close specific holes

- **No well-known names in the trust path.** The compositor starts the hub
  and gives it a private connection. Shims find the hub through the
  environment, in a directory only the user can enter, and check the hub's
  executable. The consent backend is reached through the private
  connection. Nothing can be impersonated by claiming a name first.
- **Raising power needs a compositor-drawn confirmation, answered with
  physical input.** Editing the settings file alone does nothing until
  confirmed.
- **Approved manifests are the only source of tool names and
  descriptions.**
- **D-Bus shows and takes away:** no arguments, nothing that grants.
- **The compositor owns capture and input,** so the stop key works without
  the hub, and the lock screen is out of reach.
- **Limits everywhere untrusted input arrives:**
  - **message size, before parsing:** each connection's input is read
    into a buffer of at most `MaxFrame`; a message longer than that, or
    one not ended (no newline) within it, closes that connection (its
    calls ending as for any agent or provider leaving), with nothing
    parsed or allocated past the limit. Each connection is read
    independently and without blocking, so one flooding never delays the
    others, the stop key or the compositor;
  - in-flight calls per agent and provider (`MaxInFlight`);
  - size and rate limits on provider results and notifications;
  - registrations and questions per agent (`QuestionRate`), calls per
    agent (`CallRate`);
  - the JSON parser's depth limit;
  - `RecentCalls` capped.
- **Activated providers' `Exec`** is parsed with the same quoting rules as
  `.desktop` files, and run without a shell.

## Tunables

Durations and limits are the implementation's to set, and a distribution's
to change for the experience it wants to give: the values here are
suggestions. Each says what it trades, so lowering one that guards
something is an informed choice. Format rules that clients depend on (tool
names: their characters, 48 at most) aren't tunables: changing them breaks
interoperability, not taste.

- **Where they're set:** system defaults in
  `$XDG_CONFIG_DIRS/agents/tools/settings.ini` (`[Tunables]`, from the
  distribution), the user's in their own settings file. A user's change
  that weakens a guard (a shorter pause, a higher rate limit) counts as
  raising power and needs the compositor's confirmation, as any raise does;
  a distribution's defaults (written as root) don't.

| Tunable | Suggested | Trades |
|---|---|---|
| `InputPause` | 500 ms | input ignored after a question or confirmation appears: shorter answers faster, but a click or keypress meant for something else can land on it |
| `CallDeadline` | 10 min | an absolute limit from dispatch, which progress doesn't extend: longer suits slow tools, but a stuck or stalling provider holds a call that long |
| `MaxInFlight` | 8 per agent, 8 per provider | calls running at once; more wait: higher suits busy agents, but lets one flood a provider |
| `MaxShownArgs` | 64 KiB | arguments longer than this can't be shown in a question, so such calls are refused rather than asked |
| `CallTimeout` | 30 s, reset by each progress notification | how long a provider may take before the call fails: longer suits slow tools, but an agent waits that long on a stuck one |
| `QuestionTimeout` | 2 min | an unanswered question closes as denied: longer leaves room to decide, but the agent waits |
| `ProgressInterval` | 10 s | progress sent for a call waiting on a question: shorter keeps more clients waiting, at a little traffic |
| `AwayAfter` | the compositor's idle time | when the user counts as away (questions not shown) |
| `UiTimeout` | 5 s | a UI provider that doesn't answer gives way to the next one |
| `MaxResult` | 1 MiB | a provider's result larger than this is refused: larger allows big outputs, but floods the agent's context and memory |
| `QuestionRate` | 6 a minute per agent | more questions than this are refused unasked: higher suits busy agents, but lets one wear the user down |
| `ProposeRate` | 3 a minute | `Propose` calls shown, the rest refused |
| `CallRate` | 600 a minute per agent | calls beyond this are refused |
| `HubRestarts` | 3 in 1 min | crashes before the compositor stops restarting the hub |
| `RecentCallsMax` | 100 | what `RecentCalls` returns at most |
| `AuditCut` | 4 KiB | longer strings in the audit are cut, marked so |
| `AuditKeepMax` | 1 MiB | the largest command or Lua code kept in full beside the audit |
| `MaxFrame` | 4 MiB | the largest MCP message the hub reads from an agent or a provider; a longer one, or a line not ended within it, closes that connection |
| `AskTicket` | 1 min | how long a started assistant has to connect with its ask ticket: longer suits slow starts, but a ticket waits that long to be used |
| `AskGrant` | 15 min | how long a shared assistant's grant for a window lasts (it ends sooner when the window closes): longer suits long conversations, but the app's other conversations share it that long |
| `AuditSync` | important lines | which lines are synced to disk as written (`important`, `all`, `none`): more is safer evidence, at a write's cost each |
| `AuditRotate` | 10 MiB, 3 kept | when the audit file rotates, and how many are kept |

## Flows

Each step names the interface it uses. "Audit" means a line in the audit
log; "state" means the hub's D-Bus state (`PropertiesChanged`).

### A call allowed by its level

1. The agent calls `waybar__get_battery` (MCP `tools/call`); the shim
   passes it on.
2. The hub finds the provider and the tool: approved and reachable, or the
   call fails ("no such tool", as if it weren't there).
3. The hub checks the arguments against the tool's pinned input schema
   (refused if they don't fit), then decides: the connection's level for
   the tool (its profile, its `lower`, the provider's and the tool's own
   levels); then the guards: the screen locked (the compositor's
   `Locked`), rate limits and `MaxInFlight`, the flow guard, the stopped
   state. Here: allowed.
4. The hub starts the call: for a high-impact one, an intent line in the
   audit, synced, first; `CallStarted`, `Active` (state), `SetActivity`
   (compositor); it passes the call to the provider under an id of its
   own (dispatched).
5. The provider answers within `CallTimeout` (each progress notification
   starts it again, never past `CallDeadline`): completed. Nothing heard
   in time: the provider is sent MCP's cancellation, and the call ends
   outcome unknown (see "Calls").
6. The hub checks the answer (`MaxResult`, its shape), adds `_meta`
   (provenance; untrusted where the tool says its results can carry such
   text), and records in the connection's flow record its sensitivity and
   whether it carried untrusted text.
7. The answer goes to the agent; `CallEnded` (state); audit.

### A call that asks first

1. As above, to step 3: the level is `ask`.
2. A grant that covers it (this connection, this tool or sensitivity, this
   target, not expired): it runs, as an allowed call, the grant named in
   the audit.
3. None: a question already up for this connection: the call waits behind
   it, unasked, then is decided as new (see "One question, one call").
   Otherwise the hub asks: `Ask` (compositor), with the arguments' digest,
   the question in `Pending` (state); audit (`question`). Beyond
   `QuestionRate`, refused unasked.
4. Meanwhile the hub goes on: other calls, other agents, the stop key.
5. `Answered`:
   - **deny:** the call ends with a tool result marked `isError`, "the user
     declined", so the model understands; "stop asking" denies this tool
     for the rest of the connection.
   - **once:** it runs, with exactly the arguments shown.
   - **session, timed:** a grant is made for the connection (`Grants`,
     state), and it runs.
6. While it waits, the hub sends progress for the call (every
   `ProgressInterval`). Unanswered within `QuestionTimeout`:
   `CloseQuestion`, denied, said so in the audit. The agent cancelling the
   call: `CloseQuestion` at once, the call cancelled.
7. The user away (`Away`): no question at all; the call is denied at once,
   and listed in the notice shown on return. A question already up when
   the user goes away closes, denied, the same way.

### The stop key

1. The compositor sees the key before anything else (bindings, clients),
   with or without the hub. (From a panel: `AgentHub1.Stop()`, which the
   hub passes on as `StopAssistants`; the same from here.)
2. It ends all capture and input (closes the EIS connections it handed
   out), ends the commands it ran for agents (`run_command`: each in its
   own process group, which it signals, `SIGTERM` then `SIGKILL` after a
   moment), closes its questions and confirmations, and shows that the
   assistants are stopped.
3. It sends `Stopped` (if the hub is there).
4. The hub cancels running calls: to the providers, MCP's cancellation; to
   the agents, "stopped by the user" for calls not yet dispatched, and
   **outcome unknown** for those already dispatched to a provider other
   than the compositor (a cancellation is a request; see "Calls"), which
   stay listed until the provider answers or their deadline passes. It
   revokes every grant and empties `Pending`; audit (`stop`). What the
   stop key guarantees: capture, input and the compositor's own commands
   end at once; other providers' work is asked to stop, not proven
   stopped.
5. **Assistants are now stopped, all of them:** the stopped state is the
   compositor's (its `AssistantsStopped` property), not the hub's, so it holds for
   agents connecting later, for agents reconnecting, and across a hub
   restart (the new hub reads it at `Hello`). While stopped, only `read`
   and `content` calls go through; the rest are refused, "stopped by the
   user". Programs the agents start are held to the same (their levels,
   `LevelsFor`, say so). The compositor's indicator shows it the whole
   time. It lasts until the user resumes, or the session ends.
6. **Resuming** is a click on the compositor's indicator: a confirmation
   drawn by the compositor (it raises power again), or `Propose` from a
   surface, which shows the same confirmation. Either way, once the user
   confirms, the compositor sends `Resumed()`, and the hub lifts the
   stopped state.

### A new provider

1. A manifest appears (installed, or a provider registers with an `Id`
   the hub hasn't seen), or an approved one changes.
2. The hub lists it as `pending` (`Providers`, state); audit (`provider`);
   it's offered to no agent. A notice says a program wants to offer tools,
   with "Review".
3. "Review", or Approve in the settings: `Propose` (kind `provider`).
4. The hub fetches each tool's definition from the provider (its schemas)
   and works out its names; the compositor shows `Confirm`: the
   provider's full id, its executable, each tool with its description (the
   user's language), sensitivity, reach and arguments, with a digest of
   all of it.
5. Yes: the hub pins exactly what was shown (`Pin=`: the manifest and the
   tools' definitions) and the names (`Names=`), marks it approved, and
   offers its tools; agents get MCP's `tools/list_changed`. If the
   manifest or a definition changed while the question was up, it stays
   pending for a new one. No: it stays pending, until it changes or the
   user asks again.

### Binding an agent to a profile

- **Ahead of time:** in the settings, "Add assistant": a profile and a
  label. `Propose` (kind `binding`); the compositor's confirmation shows the
  profile and the label. Yes: the hub mints a token, writes it to a file
  (mode 0600) under `$XDG_CONFIG_HOME/agents/tools/tokens/`, records its
  hash, and lists the binding in `Bindings` (its token file's path, never
  the token); the settings take the path from there and write the agent's
  configuration naming it.
- **From an agent connected as "safe":** "Bind this assistant" on its row.
  The confirmation also shows its executable and script (shown, not
  trusted). Yes: as above; the agent uses its binding from its next start.
- **Revoking:** removing the binding in the settings lowers power: at
  once, no confirmation; that agent's connections drop to "safe", and
  everything the binding allowed goes with it: its questions close
  (denied), its grants are revoked, its calls that "safe" would refuse are
  cancelled (outcome unknown where already dispatched; see "Calls"),
  calls "safe" allows go on, and the compositor gets the new levels for
  its processes (`LevelsFor`). Its token stops working at once. (A
  sandboxed agent's remembered answer revoked: its portal session and
  sockets close.)

### A level raised by editing the settings

1. The hub notices the file changed.
2. Lowered values apply at once and become the confirmed ones.
3. Raised values stay at the confirmed ones; the hub asks once for the
   edit (`Confirm`, kind `level`: each raised value a line, with their
   digest).
4. Yes: exactly the values shown apply and become the confirmed ones; any
   the file changed again meanwhile wait for a confirmation of their own. No: the file is left as
   it is (the hub never rewrites the user's file); the held values are in
   `Unconfirmed` (each key, the file's value, the confirmed value in
   force), and the settings show them "not confirmed", with a button to ask
   again (`Propose`).

### An agent leaving

1. The shim's socket closes (the agent quit, crashed or timed out).
2. The hub closes that connection's questions (`CloseQuestion`), cancels
   its calls (MCP's cancellation to the providers; dispatched ones end
   cancellation requested, then completed or outcome unknown, in the audit
   and the state), drops its grants, and tells the compositor
   (`ForgetAgent`).
3. `Agents`, `Pending`, `Grants` (state); audit (`disconnect`).

### The hub restarting

1. The hub's socket closes on the compositor's side: it closes every
   question, ends capture and input, clears the activity it showed, and
   shows the hub unavailable (keeping the stopped state, if assistants
   were stopped; see "The indicator").
2. The compositor starts the hub again (within `HubRestarts`), with new
   sockets; `Hello`; the new hub reads `AssistantsStopped`.
3. The hub reads the settings (confirmed values) and the providers, marks
   each intent in the audit with no ending as outcome unknown; audit
   (`hub`).
4. Host shims reconnect, send the agent's `initialize` (older revisions) or
   its open `subscriptions/listen` requests (2026-07-28) to the new hub
   again, and tell their agents the tools changed; calls that were running
   ended as outcome unknown (see "Calls").
5. Sandboxed agents call `ConnectAgent` again; with their `restore_token`,
   no question.

### A sandboxed agent through the portal

1. The app calls `CreateSession`, then `Start` (role `agent`, fixed for the
   session from now).
2. The portal frontend knows the app's id and calls the hub's backend
   (`org.freedesktop.impl.portal.Agent`), which accepts the call only from
   the frontend's connection.
3. A binding for that app id, remembered: answered at once with its
   profile. Otherwise the compositor asks (`Confirm`, kind `portal`): the
   app's name and id, the profile it would get, "remember". Yes, and
   remembered: the binding is written (`[Binding app:…]`), and a
   `restore_token` returned.
4. `ConnectAgent` returns a socket; MCP flows as for any agent, the hub
   knowing its app id and profile without a handshake. The socket closes
   with the session: when it's closed, when the app's bus connection goes,
   or when its remembered answer is revoked.

### A program the agent started

1. At the handshake, the hub learns the agent's process (the shim's
   parent), checks or makes its scope, and tells the compositor:
   `AgentProcess(conn, info)`, then `LevelsFor(conn, levels)`.
2. The agent runs `grim` itself, from its own shell, not through the hub.
3. `grim` asks the compositor for a screen capture, as any Wayland client.
4. The compositor finds `grim` in the agent's scope and holds it to that
   agent's `capture` level: here `ask`, so it asks, the question saying a
   program the agent started wants it.
5. Denied: the capture is refused; the compositor tells the hub
   (`ClientCall`, outcome `asked-denied`), which records it (audit, the
   connection's flow record).

### A capture through the hub's own tools

1. The agent calls `desktop__capture_window` with a window's id.
2. The hub checks the arguments, decides (levels, guards, consent as any
   call), writes the intent line, and calls `Capture(target, options)`
   (compositor), with `max_size`.
3. The compositor draws the window (leaving out private windows and the
   lock screen; refused while locked) into a sealed memfd, and returns it.
4. The hub reads the PNG, turns it into MCP image content within
   `MaxResult`, closes the memfd, marks the result (`capture`), and
   answers the agent; the audit records what was captured and its size.
   Cancelled or stopped before the compositor returned: the hub closes
   whatever it got, and the call ends as "Calls" says.

### Asking about a window

1. The user opens the window menu on a window; the compositor shows "Ask
   AI about this window", or a submenu of the asks (the hub's `SetOffers`).
2. The user clicks one: the compositor sends `AskAbout(window, ask)`.
3. The hub starts that ask's `Ask=` with a question naming the window by
   id and app id; audit (`ask`).
   - **Started for the question:** a one-time ticket in its environment.
     Its shim connects, presenting its token file and the ticket; the hub
     grants that connection `content` and `capture` for that window, while
     it lasts; the ticket is spent.
   - **Shared** (a desktop app, its URL opened): the hub grants the
     binding's connections the same, for `AskGrant` or until the window
     closes.
4. The assistant looks (`window_titles`, `window_process`, a capture of
   that window): allowed by the grant, no question. Anything else follows
   its profile (killing the window asks, as `destroy` does).

## Future-proofing

What's fixed once others build on it is the interfaces and the formats,
so each is made to grow without breaking:
- **D-Bus interfaces** are versioned by name (`AgentHub1`,
  `AgentCompositor1`) and carry a `version` property; additions raise the
  version, breaking changes make a new interface (`...2`), offered beside
  the old for a while.
- **Options and results are `a{sv}`** wherever something may be added;
  unknown keys are ignored, never an error.
- **Manifests** carry `Version=`; unknown keys are ignored; a newer major
  version is skipped (and logged), not misread. Vendors' own keys start
  with `X-`.
- **Sensitivities** may be added; an unknown one counts as `off`.
- **Capabilities are negotiated** (the compositor interface's `Hello`), so
  a hub never assumes what a compositor can do, and the reverse.
- **MCP revisions** are named, both eras supported, each side spoken to in
  its own (see "MCP revisions"); an unknown revision is refused, never
  guessed; the hub's own tools follow MCP's annotations.
- **Windows are named by a Wayland standard** (`ext-foreign-toplevel-list`),
  not by any compositor's own ids.

## Names

The spec uses the names proposed for sharing: `org.freedesktop.AgentHub1`,
`org.freedesktop.AgentCompositor1`, `org.freedesktop.portal.Agent`,
`org.freedesktop.impl.portal.Agent`, `org.freedesktop.AgentUI1`.
- **The peer-to-peer interface** (`AgentCompositor1`) has no bus name and
  ships under its proposed name from the start.
- **The desktop-file interfaces** (`org.freedesktop.AgentClient1`,
  `org.freedesktop.AgentProvider1`, in `Implements=`) name no bus or
  object either, and ship under their proposed names from the start: apps
  write them into their desktop files, which a rename would leave behind.
- **On the session bus,** the hub ships its state as `org.waybox.AgentHub1`
  until the names are agreed. Agent UI has no bus name (it runs over each
  provider's registration socket), so only its interface name changes: the
  hub calls whichever of the two a provider exports.
- **The portal** lives in xdg-desktop-portal's frontend, so it needs that
  project's agreement. Until then a small interim frontend offers the
  same interface at `org.waybox.portal.Agent`, which a sandboxed agent
  reaches with one `--talk-name`. It is a bus connection of its own (a
  process of its own, or a connection the hub opens only for it),
  owning only that name and exporting only that interface, so the
  `--talk-name` reaches nothing else of the hub's. It does the portal
  frontend's checks itself (see "How the caller's app id is known") and
  talks to the hub privately, not over the bus.

### Moving to the shared names

A rename isn't free: surfaces find the hub by its bus name and object
path, and sandboxed apps hold a `--talk-name` for the interim portal's
exact name. So when the shared names are agreed:
- **The hub's state answers to both** for a transition period: it owns
  `org.freedesktop.AgentHub1` and `org.waybox.AgentHub1`, and exports the
  interface under both names at both object paths, on the state's own
  connection (never on a sandbox-facing one). Surfaces look for the shared
  name first and fall back to the old one; the `version` property tells
  them what they're talking to. The old one carries `Deprecated` (true)
  and is logged when used.
- **The portal moves into xdg-desktop-portal,** reachable by every sandbox
  without any `--talk-name`. Apps ask `org.freedesktop.portal.Desktop` for
  the Agent interface (its `version`) first, and use the interim name only
  where it's missing. The interim frontend keeps running, alone on its own
  connection as before, through the transition; remembered answers and
  restore tokens are the hub's, so they carry over between the two.
  Apps drop the `--talk-name` from their manifests when they stop
  supporting desktops without the portal.
- **The transition lasts at least twelve months** after a release ships
  the shared names (the window MCP gives its own deprecations), announced
  in the release notes; the old names are then removed in a release that
  says so.

## Open questions

- **What else the hub carries:** it offers tools only. MCP's resources and
  prompts, its tasks extension, and remote MCP servers as providers are
  left for a later version, each once it can be held to the same levels,
  consent and call states as tools.
- **Classifying tools:** a provider declares its own sensitivity, and
  approving it is where the user decides whether to believe that. Whether
  the hub should also recognize well-known kinds of tools itself is open.
- **Where to propose it, and in what parts:** the hub's state interface,
  the compositor interface, the Agent portal and Agent UI could each be
  proposed on its own; Agent UI in particular is useful without the rest.
  Best done once this has been in use for a while.
