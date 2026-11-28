# Agent tools: a desktop broker for AI assistants

*Draft, third pass: Waybox's implementation target, written as the
standard we propose. Interface names are the proposed shared ones
(`org.freedesktop.*`); see "Names" for what ships before they're agreed. It supersedes the
plan in #196; the decisions on the first pass's questions are at the end,
with the order it's built in.*

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
- **The compositor supervises it:** restarted with a growing delay; after
  too many crashes in a short time (a tunable), not again, and the user is
  told (a notice and the indicator).
- **When the hub's socket closes,** the compositor closes every open
  question, ends capture and input, and lowers the indicator: all safe
  even if the hub was in a bad state.
- **What survives a restart:** the settings, approvals, bindings and the
  audit (files). Grants and open questions don't: they end, the safe way.
- **Sandboxed agents** (through the portal) can't reconnect by themselves:
  their socket closes, and they call `ConnectAgent` again; with their
  `restore_token`, without a question.
- **Shims reconnect by themselves,** redo the MCP handshake with the new
  hub, and tell the agent its tools changed; calls that were running get
  "the desktop's hub restarted; call again".
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

**Why a manifest at all, when MCP lists tools:**
- **Before the provider runs.** The user sees and approves a provider and
  its sensitivities before it ever runs.
- **A declaration, checked at runtime.** It is the provider's promise, and
  the hub holds it to that: a tool not in the manifest, or one whose MCP
  annotations contradict the manifest, is refused and logged.
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
- **short names at most 16 characters;** a name still too long has its
  tool part cut and 4 characters of a hash of the full name added, so it
  stays unique and the same from one session to the next.

The audit and the questions always show the full names (the provider's
`Id` and the tool's own name), never the shortened one.
- **Reserved:** the compositor's short name (`waybox`) belongs to the
  compositor's provider.
- **Unique:** short names are unique; a second provider whose `Id` would
  shorten to a name in use gets its full id.
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
- **Who it is, for the record:** the hub reads the shim's credentials from
  the socket (`SO_PEERCRED`, or a pidfd where the system has one), then the
  shim's parent, the agent: its executable and the script on its command
  line, shown and audited, never trusted.
- **A version the hub doesn't speak** gets `{"ok": false, "error": …}`
  naming the versions it does, and the connection closes.
- After this, MCP flows both ways. The agent's `clientInfo` from MCP's
  `initialize` is recorded as its claimed name.

The portal's sockets skip the handshake: the portal has told the hub the
app's id and its profile.

**Hub to provider:** MCP over a Unix stream socket.
- **The calls:** tools, resources, and notifications for state that
  changes (media playing, battery).
- **Activated providers** get the socket as their standard input and
  output, or as a passed file descriptor.
- **Registering providers** connect to `$XDG_RUNTIME_DIR/agents/tools/hub`.

### Requests from providers: sampling and elicitation

MCP lets a server ask its client too: **sampling** (run a prompt on the
agent's model) and **elicitation** (ask the user something through the
agent's UI). Through the hub, both would let a provider spend the user's
model, learn what the agent knows, push text at the model, or question
the user in the agent's name.

- **Not passed on, by default:** a provider sending either gets "not
  supported", and the attempt is in the audit.
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
is also marked, in its `_meta` (`"org.waybox/untrusted": true`), for agents
that understand the marking. The text itself isn't rewritten: a result
must stay what the provider returned.

**Presets:**

| | read | content | act | destroy | exec | capture | input | ui |
|---|---|---|---|---|---|---|---|---|
| safe (default) | allow | allow\* | off | off | off | off | off | ask\*\* |
| trusted | allow | allow | allow | ask | ask | ask | off | allow |
| full | allow | allow | allow | allow | allow | allow | allow | allow |

\* In "safe": the accessibility provider (`atspi`) is `off` by name; other
`content` tools (window titles) are allowed.

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
Preset=safe
StopKey=Super+Escape

# Per sensitivity, over the preset
[Levels]
act=ask

# Each on unless "off"
[Guards]
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
Pin=sha256:…
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
same. (An agent with a shell can rewrite both files; that's the
same-user limit under "The limits". The point is that no tool call,
setting or sandboxed agent can raise power by itself.)

**Agent profiles:** the settings can define profiles, for example "local
model: full" and "cloud assistant: safe".
- **Bound with a token:** each binding of an agent to a profile has a
  token, minted when the user confirms the binding in a compositor-drawn
  confirmation.
  - **Where it goes:** into a file of its own (mode 0600), which that
    agent's MCP configuration names: `waybox-mcp --token-file PATH`. Never
    on the command line: every user of the machine can read command lines
    (`ps`). The hub keeps only its hash, and the binding can be revoked in
    the settings.
  - **Why not the executable:** most assistants are interpreted. Claude
    Code runs as `node`, many others as `python3`, so the executable the hub
    sees is the interpreter, which every script shares. (The prototype
    logged its agent as `/usr/bin/python3.14`.)
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
- consent prompts (`ask`);
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
- **The hub remembers, per agent connection,** which sensitivities and
  providers it has had data from.
- **The flow guard:** once a connection has had `content` or `capture` data,
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
- **The provider's Name** from its approved manifest is shown, not its id.
- **The arguments** are summarized, so the user agrees to *this* command, not
  to "commands".

**Agent-supplied text is sanitized:** arguments and self-declared names lose
control and bidirectional characters, are cut to a fixed length, and are shown
as literal code, visibly apart from the compositor's own words.

**The dialog resists rushed or tricked answers:**
- input is ignored for a moment after it appears (a tunable), so a click
  meant for something else can't land on "Allow";
- **Deny** is the default, so Enter never allows;
- questions are rate-limited per agent;
- after repeated denials, the user can say "stop asking".

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
  outcome; never the arguments.
- Property changes come as D-Bus's standard `PropertiesChanged`, so there
  are no signals of its own for providers or grants changing.

**Methods:**
- `Revoke(grant)`
- `RevokeAll()`
- `Stop()`: what the stop key does, from a panel button
- `RefuseProvider(id)`
- `RecentCalls(count)`: the latest calls, metadata only, with `count`
  capped (a tunable)
- `Propose(change)`: ask the compositor to show the confirmation for a
  change that raises power (approve a provider, bind an agent, raise a
  level, resume after a stop). It only asks: the user's physical answer
  decides, so calling it grants nothing. Rate-limited and logged.

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
right-click is `RevokeAll`. The prototype's `surface.py` is such a script.

**Short calls are too brief to show.** Most calls start and end within
milliseconds, so `Active` flickers: an indicator follows `Watching`
(capture or input) and grants, not `Active`.

**Pending providers deserve a state of their own** on a surface (a new
program wants to offer tools), so the user notices and decides.

## The audit log

The hub appends one JSON line per event to
`$XDG_STATE_HOME/agents/tools/audit.log`:

```json
{"v":1,"time":"2026-11-28T10:01:02.123Z","event":"call",
 "agent":{"conn":17,"profile":"trusted","claim":"Claude Code","label":"laptop",
          "binding":"sha256:3f2a…","remote":false},
 "provider":"org.waybar.Waybar","tool":"media_play_pause","name":"waybar__media_play_pause",
 "sensitivity":"act","args":{},"outcome":"asked-allowed","scope":"session",
 "why":"level","ms":12,"result":{"bytes":31}}
```

- **`event`:** `call`, and the others the hub does: `connect`,
  `disconnect`, `question`, `confirm`, `grant`, `revoke`, `stop`,
  `provider` (pending, approved, refused, changed), `settings` (raised,
  lowered), `refused-request` (sampling or elicitation), `hub` (started,
  restarted).
- **`outcome`** (calls): `allowed`, `asked-allowed`, `asked-denied`,
  `refused` (a level, a guard, a policy), `failed`, `cancelled` (stop,
  revoke, the agent leaving).
- **`why`:** what decided it: `level`, `grant`, `flow-guard`, `policy`,
  `locked`.
- **Arguments** with secrets redacted (each tool's `Secret`; synthetic
  input into password fields as its length only); a string longer than
  the audit's cut length (a tunable) is cut, marked so.
- **For `capture`:** what was captured (window or output, size); **for
  `input`:** what was typed.
- **Rotation:** past a size (a tunable) the file becomes `audit.log.1`,
  a few kept (a tunable).
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
- **the compositor interface,** answered only on the hub's socket;
- **its tools,** as an MCP provider with a manifest;
- **confirmations and questions,** drawn by itself, answered by physical
  input only;
- **the stop key,** always;
- **the indicator,** while capture or input is active. Switchable; a
  surface may show one too.

### The `waybox` provider's tools

| Tool | Sensitivity | What it does |
|---|---|---|
| `list_windows` | read | windows without their titles: id, app id, desktop, output, geometry, state, focused; each window's process id and whether it answers (Waybox's ping) |
| `window_titles` | content | each window's id and title |
| `list_desktops`, `list_outputs`, `get_version` | read | desktops and their names; monitors, modes, scale; Waybox's version |
| `window_process` | read | a window's process: its state, CPU use, what it's waiting on, its children (the stuck-window case) |
| `check_config` | read | Waybox's parser on a proposed rc.xml, without applying it |
| `focus_window`, `move_window`, `resize_window`, `set_window_state`, `send_to_desktop`, `switch_desktop` | act | as their actions do |
| `close_window` | destroy | asks the window to close, as its close button |
| `kill_window` | destroy | ends a window's process (Waybox's Kill), for one that doesn't answer |
| `run_action` | exec | any rc.xml action by name (Execute among them) |
| `run_command` | exec | a command, as Execute runs it |
| `write_config` | exec | rc.xml replaced (backed up, checked first, then Reconfigure): it can run commands |
| `run_lua` | exec | Lua run in Waybox (when built with Lua), the code in the audit in full |
| `capture_window`, `capture_output` | capture | an image of a window or a monitor, drawn by Waybox |
| `type_text`, `press_keys`, `click` | input | synthetic input into the granted window |

**Not offered: raw IPC.** It would be every typed tool without the typing,
and the audit would only see "raw".

**The accessibility tree** comes from a provider of its own, `atspi`
(AT-SPI, the accessibility bus): `content`, `off` in "safe".

## Scripts as providers

Waybox's Lua scripts offer tools with `wb.mcp_tool(...)`. To the hub,
the `waybox` provider simply has more tools, each declaring its sensitivity
in the script.

A policy hook, `wb.policy("mcp", fn)`, sees every call routed through the
compositor provider, and can refuse it.

## Agents in practice

**Most agents are host programs with a shell.** Distributions package them
as ordinary programs (Fedora: `pi-coding-agent`, `openclaw`,
`hermes-agent`, Emacs's `agent-shell`), often with a local model
(`ollama`, `llama-cpp`). For these, a binding with a token file is the way
in, and the levels confine them only as far as they're otherwise confined
(see "The limits"). The Agent portal is for sandboxed agents, and for
whoever wants an agent truly confined.

**Recommended:** an agent's own command tool off, or the agent sandboxed,
wherever the work allows it; the settings offer it when adding an
assistant, through its integration file (`ShellOff=`, below).

**Their models may be anywhere:** on the machine, on the network (a model
server on the LAN), or a cloud service. Only the agent's client talks to
the hub, so where the model runs doesn't change what the agent may do.

### Agent integrations

Each agent keeps its MCP servers in its own place and format: a JSON file
here, a command there (`openclaw mcp add`, `claude mcp add`). So adding the
hub to an agent is described by an **agent integration file**, which the
settings use for "Add assistant" (bind it, then write its configuration):

```ini
# $XDG_DATA_DIRS/agents/clients/openclaw.ini
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
```

- **Who ships them:** the agent's package, the distribution, or the
  desktop; the user's own go in `$XDG_DATA_HOME/agents/clients/`.
- **The shim's command** (`%s`) is the desktop's (`waybox-mcp --token-file
  …`), so one file serves every desktop.
- **A JSON file is edited in place,** the rest of it left as it was, and
  backed up first; a command is run without a shell.
- **Unknown agents** still work: the settings show the command to add by
  hand.

### Unattended agents

Some agents act on their own: on a schedule, or "proactively" (OpenClaw's
word), while nobody is at the desk.
- **While the user is away** (idle, or the screen locked; the compositor's
  `Away`), questions aren't shown: there's no one to answer, and a waiting
  call would outlast its client's timeout. The call is denied at once with
  a tool error the model understands ("the user is away; nothing was
  done").
- **On return,** a notice lists what was asked meanwhile (agent, tool,
  when), with "Review": grants made then apply to the agent's next calls.
- **A profile can set levels for while the user is away** (`Away=read`:
  read and content only), so an assistant can keep watching without acting.

### Clients' timeouts

Agents give up on a call after their own timeout: from seconds to a day,
depending on the client and its settings. A question can outlast it.
- **While a question waits,** the hub sends MCP progress notifications for
  the call (every `ProgressInterval`); clients that count progress as
  activity keep waiting.
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
  (from the shim's parent), `AgentProcess(conn, pidfd)`.
- **On Linux, the agent's processes are kept together:** the hub asks the
  user's service manager to move the running agent into a scope of its own
  (systemd's `StartTransientUnit` with the agent's process); everything it
  starts stays in that scope, even once its parent exits. The compositor
  reads a client's scope from its process. **Elsewhere,** the parent chain
  (weaker: a process whose parent exits is adopted by another).
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

**Identity:** the portal knows a sandboxed app's id for certain (Flatpak
and Snap both make it known, and every portal relies on it), and passes it
to the hub. A sandboxed agent's profile is bound to its app id: no token
to copy.

**The backend is the hub** (`org.freedesktop.impl.portal.Agent`): the
portal frontend checks the caller and hands the hub the app id; the
compositor asks the question, through the compositor interface, as for
any other.

**Manifests from inside a sandbox:** an app ships its manifest in
`share/agents/tools/`, and Flatpak exports it as it exports desktop files,
icons and search providers (an addition to Flatpak's exported folders).
Until Flatpak does, the hub reads installed Flatpaks' manifests from their
deployments, each `Id` checked against its app's id.

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
- **Every session and restore token is bound** to that app id and to the
  sender's unique bus name; a call from another sender, or another app id,
  with a session handle or a restore token it didn't get, is refused and
  logged. A provider's manifest `Id` must be that app id.

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
its manifest which kinds it renders:

```ini
[UI]
Kinds=choose,ask
```

The user picks a provider per kind, with fallbacks. Where standards already
cover a kind, the hub uses them directly:
- **notify:** freedesktop Notifications, with its actions;
- **pick_file:** the portal FileChooser. It's sandbox-aware, and the user's
  pick is itself the consent to share the file. The chooser grants access
  to its caller, the hub, so what reaches the agent depends on it:
  - **A host agent** gets the path.
  - **A sandboxed agent** (through the Agent portal) gets the file through
    the document portal: the hub adds it to the document store for the
    agent's app id only (`AddFull`, read for opening, read and write for
    saving), and returns the path the app sees inside its sandbox (under
    `/run/user/…/doc/`). Another app can't use it.
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
  answer within the UI provider timeout (a tunable) is treated as failed. The hub then uses the
  next provider for that kind (Waybox's built-in ones last), and the agent
  gets a plain error, never a hang.
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
    from focusing its new window (the prototype's question opened focused,
    its text selected). So providers tag their windows `agent-ui`
    (xdg-toplevel-tag), and the compositor has a built-in rule: windows
    with that tag never get focus when they appear, and open beside, not
    over, the focused window.
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
  - **A secret question's prompt isn't secret** and is logged. The prototype
    first redacted it, which hid what a phishing attempt had asked.
  - **Files:** a file picked in the chooser goes to the agent because the
    user picked it.

## Threat model and security

### Who the design defends against

| Attacker | Example | Defended by |
|---|---|---|
| **A prompt-injected agent** (the main threat) | a web page or window title telling the model to run commands | levels, consent, grants scoped to the agent, the stop key, the audit |
| **A malicious or compromised provider** | a tool that changes its description to steer the model | approval of the manifest, pinned descriptions, reserved and unique names |
| **Another program of the user** | an app trying to grant itself access | nothing it can do *through this design* gives it more than it had; see below |
| **A sandboxed app** | a Flatpak with session bus access | no arguments on D-Bus, nothing that grants over D-Bus, no access to the compositor's private connection |
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
- **A confined agent** (its shell off, or a sandbox whose only way out is
  the hub): the levels are everything it can do.

For real confinement, run the agent so: in a sandbox where the hub's
socket (or the Agent portal) is the only way out, or with its own command
tool turned off.

**Data can flow between providers through the agent.** Content read from
one tool (an accessibility tree, notification text) can be passed as
arguments to another provider's tool, which might send it off the machine.
Levels per provider (keeping a networked provider at `ask`) and the audit
are the defence. Rules about where data may flow are out of scope for now.

**Read results can carry injections.** Window titles and other text from
applications reach the model, and are allowed even in "safe". Results
are marked as untrusted content, for agents that understand the marking
(decision 2); for those that don't, the hub's instructions say so.

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
  - size and rate limits on provider results and notifications;
  - registrations and questions per agent;
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
3. The hub decides: the connection's level for the tool (its profile, its
   `lower`, the provider's and the tool's own levels); then the guards: the
   screen locked (the compositor's `Locked`), rate limits, the flow guard,
   the stopped state. Here: allowed.
4. The hub starts the call: `CallStarted`, `Active` (state),
   `SetActivity` (compositor); it passes the call to the provider under an
   id of its own.
5. The provider answers within `CallTimeout` (each progress notification
   starts it again), or the call fails and the provider is sent MCP's
   cancellation.
6. The hub checks the answer (`MaxResult`, its shape), adds `_meta`
   (provenance; untrusted where it carries application text), and records
   the sensitivity in the connection's flow record.
7. The answer goes to the agent; `CallEnded` (state); audit.

### A call that asks first

1. As above, to step 3: the level is `ask`.
2. A grant that covers it (this connection, this tool or sensitivity, this
   target, not expired): it runs, as an allowed call, the grant named in
   the audit.
3. None: the same question already waiting (the same connection, tool and
   target): the call waits on that one. Otherwise the hub asks: `Ask`
   (compositor), the question in `Pending` (state); audit (`question`).
   Beyond `QuestionRate`, refused unasked.
4. Meanwhile the hub goes on: other calls, other agents, the stop key.
5. `Answered`:
   - **deny:** the call ends with a tool result marked `isError`, "the user
     declined", so the model understands; "stop asking" denies this tool
     for the rest of the connection.
   - **once:** it runs.
   - **session, timed:** a grant is made for the connection (`Grants`,
     state), and it runs.
6. While it waits, the hub sends progress for the call (every
   `ProgressInterval`). Unanswered within `QuestionTimeout`:
   `CloseQuestion`, denied, said so in the audit. The agent cancelling the
   call: `CloseQuestion` at once, the call cancelled.
7. The user away (`Away`): no question at all; the call is denied at once,
   and listed in the notice shown on return.

### The stop key

1. The compositor sees the key before anything else (bindings, clients),
   with or without the hub.
2. It ends all capture and input (closes the EIS connections it handed
   out), closes its questions and confirmations, and shows that the
   assistants are stopped.
3. It sends `Stopped` (if the hub is there).
4. The hub cancels running calls (the agents get "stopped by the user",
   the providers MCP's cancellation), revokes every grant, empties
   `Pending`; audit (`stop`).
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
4. The compositor shows `Confirm`: the provider's full id, its executable,
   each tool with its description (the user's language) and sensitivity.
5. Yes: the hub pins the manifest's content (`Pin=`), marks it approved,
   and offers its tools; agents get MCP's `tools/list_changed`. No: it
   stays pending, until the manifest changes or the user asks again.

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
  once, no confirmation; that agent's connections drop to "safe".

### A level raised by editing the settings

1. The hub notices the file changed.
2. Lowered values apply at once and become the confirmed ones.
3. Raised values stay at the confirmed ones; the hub asks once for the
   edit (`Confirm`, kind `level`: each raised value a line).
4. Yes: they apply and become the confirmed ones. No: the file is left as
   it is (the hub never rewrites the user's file); the held values are in
   `Unconfirmed` (each key, the file's value, the confirmed value in
   force), and the settings show them "not confirmed", with a button to ask
   again (`Propose`).

### An agent leaving

1. The shim's socket closes (the agent quit, crashed or timed out).
2. The hub closes that connection's questions (`CloseQuestion`), cancels
   its calls (MCP's cancellation to the providers), and drops its grants.
3. `Agents`, `Pending`, `Grants` (state); audit (`disconnect`).

### The hub restarting

1. The hub's socket closes on the compositor's side: it closes every
   question, ends capture and input, lowers the indicator.
2. The compositor starts the hub again (within `HubRestarts`), with new
   sockets; `Hello`.
3. The hub reads the settings (confirmed values) and the providers; audit
   (`hub`).
4. Host shims reconnect, redo MCP's handshake, and tell their agents the
   tools changed; calls that were running got "the desktop's hub
   restarted; call again".
5. Sandboxed agents call `ConnectAgent` again; with their `restore_token`,
   no question.

### A sandboxed agent through the portal

1. The app calls `CreateSession`, then `Start` (role `agent`).
2. The portal frontend knows the app's id and calls the hub's backend
   (`org.freedesktop.impl.portal.Agent`).
3. A binding for that app id, remembered: answered at once with its
   profile. Otherwise the compositor asks (`Confirm`, kind `portal`): the
   app's name and id, the profile it would get, "remember". Yes, and
   remembered: the binding is written (`[Binding app:…]`), and a
   `restore_token` returned.
4. `ConnectAgent` returns a socket; MCP flows as for any agent, the hub
   knowing its app id and profile without a handshake.

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
- **MCP versions** are negotiated as MCP does; the hub passes on what both
  ends speak, and its own tools follow MCP's annotations.
- **Windows are named by a Wayland standard** (`ext-foreign-toplevel-list`),
  not by any compositor's own ids.

## Names

The spec uses the names proposed for sharing: `org.freedesktop.AgentHub1`,
`org.freedesktop.AgentCompositor1`, `org.freedesktop.portal.Agent`,
`org.freedesktop.impl.portal.Agent`, `org.freedesktop.AgentUI1`.
- **The peer-to-peer interface** (`AgentCompositor1`) has no bus name and
  ships under its proposed name from the start.
- **On the session bus,** the hub ships its state as `org.waybox.AgentHub1`
  (and Agent UI as `org.waybox.AgentUI1`), with the same members, until the
  names are agreed; renaming then changes nothing else.
- **The portal** lives in xdg-desktop-portal's frontend, so it needs that
  project's agreement. Until then the hub offers the same interface at
  `org.waybox.portal.Agent`, which a sandboxed agent reaches with one
  `--talk-name`; the portal frontend's checks (the caller's app id) are
  done by the hub itself, as the frontend does.

## Decisions on the first pass's questions

1. **Sensitivity is self-declared.** The hub doesn't classify tools itself
   for now: approving a provider is where the user decides whether to
   believe what it declares.
2. **Content reaching the model.** Marked: in the hub's instructions, and
   in each result's `_meta` (see "Sensitivity and levels"); never
   rewritten.
3. **Who starts the hub.** Under Waybox, Waybox, with the private
   connection. Elsewhere a systemd user unit, documented but not built, and
   limited to `read` and `content` (no trusted consent without a
   compositor).
4. **Sandboxed agents.** In scope (third pass): the Agent portal, which is
   also the recommended way to confine an agent.
5. **Resource streams.** Tools only at first; resources and subscriptions
   later.
6. **The name.** The spec uses the proposed shared names; what ships before
   they're agreed is under "Names".
7. **The consent route on other desktops.** The compositor interface
   (third pass): every compositor draws its own questions, as Waybox does;
   no portal Access backend is used.
8. **Agent UI as a standard of its own.** Later, likewise.

Also decided in the second pass:
- **Settings** in a file of their own (`agents/tools/settings.ini`), not rc.xml.
- **`ui` at `ask` in "safe"**, once per agent connection.
- **The stop key:** Super+Escape by default, changeable, never unbound.
- **Third pass:** the hub desktop-neutral, the compositor interface
  (peer-to-peer D-Bus) for consent, capture, input and the stop key; the
  compositor's tools as an ordinary provider; capture as files and input
  through libei (EIS), not through the ScreenCast and RemoteDesktop
  portals; MCP's tool annotations implied by sensitivities; `McpName`
  linking a manifest to its `server.json`; unknown sensitivities `off`;
  windows named by `ext-foreign-toplevel-list`; the Agent portal.
- **Tenth pass (review 21):** the stopped state the compositor's, holding
  for new and reconnecting agents and across hub restarts; the caller's app
  id established from the D-Bus sender's pidfd (Flatpak's `.flatpak-info`,
  Snap's label), sessions and restore tokens bound to it, the interim
  name's `--talk-name` stated as the exception; `list_windows` without
  titles and `window_titles` (content); `Reach=local|outside` apart from
  sensitivity, giving `openWorldHint` and keying the flow guard; picked
  files reaching sandboxed agents through the document portal; grants
  naming their connection; the audit's durability (`AuditSync`).
- **Ninth pass:** the agent's processes held to its levels by the
  compositor (a systemd scope on Linux, the parent chain elsewhere:
  capture, virtual input, clipboard, window management and IPC from
  programs the agent starts; `AgentProcess`, `ClientCall`); the limits
  said plainly for unconfined and confined agents; agents' own command
  tools off or sandboxed recommended (`ShellOff=` in integration files);
  the review's gaps: `Resumed()`, `Bindings`, `Unconfirmed`.
- **Eighth pass:** one `agents/` folder per XDG location (`tools/`,
  `clients/`, beside Agent Skills' `skills/`); existing MCP servers as
  providers, a dispatching tool at its worst case's sensitivity; the
  desktop's skill shipped with the hub; skills that act going through the
  hub's tools.
- **Seventh pass:** the flow guard keeps window titles (they carry
  injections) but asks about the flow once, "for this session" offered
  whatever the sensitivity; agents in practice (host agents first);
  agent integration files for adding the hub to an agent's configuration;
  unattended agents (no questions while away, denied with a notice on
  return; `Away=` levels in a profile); progress sent while a question
  waits, a client's cancel closing it; how this relates to ACP's
  permission requests, and the `asks` hint for frontends.
- **Sixth pass:** durations and limits are tunables (suggested values,
  what each trades; a user weakening a guard confirms it, a distribution's
  defaults don't); the flows written out; the compositor's `Locked`;
  questions shared by identical calls, closed when unanswered or when the
  call is cancelled; a denial reaching the model as a tool error; the
  stopped state after the stop key, resumed by a confirmation; `Propose`
  for the settings and surfaces to ask for a confirmation; binding ahead
  of time or from a connected agent; the hub never rewriting the user's
  file; the portal backend and its confirmation kind.
- **Fifth pass:** the shim's handshake (the token from a file, never the
  command line; `lower`; `label`); the settings file's format under
  `$XDG_CONFIG_HOME/agents/tools/`, raises held at the confirmed values
  until confirmed; the audit's line format and rotation; the state
  interface's signals as `PropertiesChanged`.
- **Fourth pass:** tool names `<provider>__<tool>`, lowercase, at most 48
  characters; the hub supervised by the compositor (what survives a
  restart, what doesn't); sampling and elicitation not passed on (later,
  sensitivities of their own); translations in manifests, the agent
  always getting the approved untranslated text; provenance on every
  result and the flow guard; no network listener, remote clients through
  a local bridge marked `remote`.
- **From #196:** AT-SPI (`content`), `check_config` (`read`),
  `write_config`, `run_action` and `run_lua` (`exec`), and the process
  tools for a stuck window (`read`; Kill as `destroy`). Raw IPC is dropped.

## The order it's built in

One pull request each, each useful on its own; tests on private buses
and a headless Waybox, never the user's session.

1. **The hub and the shim, started by Waybox:** the two sockets (the
   compositor interface's `Hello`; the compositor's tools as MCP); the
   `waybox` provider's `read` and `content` tools (the process id and
   whether a window answers in its list); the audit log. Every agent gets
   "safe", with `ask` acting as `off` until step 2.
2. **Consent and the stop key:** questions drawn by Waybox (physical input
   only, a pause before input counts, Deny the default); grants per
   connection; the D-Bus state for surfaces; `act` and `destroy` tools at
   `ask`.
3. **Settings, profiles and tokens:** `agents/tools/settings.ini`, the confirmation
   for anything that raises power, wbconf's page.
4. **Config and command tools:** `check_config`, `write_config`,
   `run_command`, `run_action`.
5. **Providers of other programs:** manifests, activated and registering
   providers, approval and pinning.
6. **The Agent portal:** sandboxed agents and providers, at
   `org.waybox.portal.Agent` until the shared portal is agreed.
7. **The accessibility provider** (`atspi`).
8. **Scripts:** `wb.mcp_tool`, `wb.policy("mcp")`, `run_lua`.
9. **Agent UI:** the `ui` tools, Waybox's fallbacks, the `agent-ui` rule.
10. **Capture and input** (`Capture`, `OpenInput` with EIS), with the
    indicator.

## Still open

- **When to propose this outside Waybox,** and as what: the hub's D-Bus
  state, Agent UI and a consent portal are each candidates. After it has
  been in use for a while.
