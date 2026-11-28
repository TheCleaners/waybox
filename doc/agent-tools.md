# Agent tools: a desktop broker for AI assistants

*Draft, second pass: Waybox's implementation target. Not a standard; a
proposal meant to become one once it has proved itself. It supersedes the
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
| **Consent UI** | asks the user; trusted, drawn by the compositor | a portal backend |

The compositor plays three of these roles: a provider (windows, desktops,
outputs), the consent UI, and the only party able to capture the screen,
synthesize input, or enforce the stop key.

```
 agent ──stdio MCP── shim ──┐
 agent ──stdio MCP── shim ──┤
                            hub ── providers (MCP over sockets):
                             │       compositor, panel, launcher, scripts
          D-Bus state ───────┤
          (surfaces)         └── consent (portal Access) ── compositor UI
```

**Why a hub and shims, not one server per agent:**
- **Two agents at once.** An agent starts its MCP server itself, so two
  agents would otherwise get two brokers, with two audit logs and two ideas
  of what's granted.
- **One state for surfaces.** There's no single state for a panel to show
  without the hub.

The hub is one per session, **started by the compositor**:
- **A private connection:** the compositor gives the hub a connection of its
  own (a socket inherited at launch). It is the only channel through which
  consent, capture and input are requested. The general IPC socket can't
  reach them.
- **Where shims find the hub:** the socket's path is in the session's
  environment, in a directory only the user can enter.
- **Independent of IPC:** the private connection works with Waybox's
  general IPC socket turned off (its default), so none of this needs IPC
  enabled.
- **On other desktops:** a systemd user unit, or the desktop's own session
  startup. Documented, not built: without a compositor's private
  connection, such a hub has no consent it can trust, so it offers no tool
  above `read` and `content`.

There are no well-known names in the trust path that another program could
claim first.

## Providers

### The manifest

A provider installs a manifest in `$XDG_DATA_DIRS/agent-tools/` (system,
from a package) or `$XDG_DATA_HOME/agent-tools/` (the user's):

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

**Tool fields:**
- **Description:** what's shown to the user when approving, and the *only*
  description agents ever see. Tool names and descriptions reach the model,
  so they're an injection channel: the hub never passes on what the provider
  says over MCP. A live description that differs from the approved one is
  ignored and logged.
- **Sensitivity:** how dangerous the tool is. The levels key on it (below).
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

**Sandboxed providers** (Flatpak): register through a portal and are
identified by their app id.

**New providers need the user's approval.** A provider seen for the first
time, or one whose manifest changed, is listed in the hub's state as
pending. It is offered to no agent until the user approves it:
- **How it's approved:** in a confirmation the compositor draws, showing the
  full id, the executable, and each tool's description and sensitivity.
- **The approval is pinned** to the manifest's content: any change makes the
  provider pending again.
- **Over D-Bus a provider can only be refused,** never approved.

### Naming

Agents see tools as `<provider>.<tool>`, with the provider's short name
from its `Id`: `waybar.get_battery`, `waybox.list_windows`. That avoids
collisions and tells the model, and the audit, whose tool it is.
- **Reserved:** the compositor's short name (`waybox`) belongs to the
  compositor's provider.
- **Unique:** short names are unique; a second provider whose `Id` would
  shorten to a name in use gets its full id.
- **No impersonation:** `com.evil.Waybox` can't appear as `waybox.close_window`.

### Transport

**Agent to hub:** the shim passes MCP lines both ways.
- **The agent is done sending:** the shim half-closes its side of the hub
  socket and keeps passing on answers still to come.
- **The hub then closes,** once answers are sent. An agent that leaves with
  calls in flight is normal: its open questions are closed, and nothing
  waits on it.

**Hub to provider:** MCP over a Unix stream socket.
- **The calls:** tools, resources, and notifications for state that
  changes (media playing, battery).
- **Activated providers** get the socket as their standard input and
  output, or as a passed file descriptor.
- **Registering providers** connect to `$XDG_RUNTIME_DIR/agent-tools/hub`.

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

\* In "safe": window titles only; accessibility trees are `off`.

\*\* In "safe", an agent's first `ui` request is asked about once, for that
agent's connection: Agent UI never takes focus and is always marked, but a
prompt-injected agent could still use it to phish.

**Where the settings live:** `$XDG_CONFIG_HOME/waybox/agent-tools.ini`, a
file of its own, not rc.xml: wbconf and scripts edit rc.xml all the time,
while this file's raises count only after the compositor's confirmation
(below). Lowering takes effect at once.

**Agent profiles:** the settings can define profiles, for example "local
model: full" and "cloud assistant: safe".
- **Bound with a token:** each binding of an agent to a profile has a
  token, minted when the user confirms the binding in a compositor-drawn
  confirmation.
  - **Where it goes:** into that agent's MCP configuration, as
    `waybox-mcp --token …`. The hub keeps only its hash, and the binding
    can be revoked in the settings.
  - **Why not the executable:** most assistants are interpreted. Claude
    Code runs as `node`, many others as `python3`, so the executable the hub
    sees is the interpreter, which every script shares. (The prototype
    logged its agent as `/usr/bin/python3.14`.)
  - **What the confirmation shows:** the executable and the script from the
    command line (`node …/cli.js`), so the user knows what they're binding.
    The hub records them for display and the audit, never as authority.
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
- the on-screen indicator while capture or input is active (turning it off
  warns: the compositor's indicator is the one that can't be faked, see
  below);
- the audit log.

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

**The hub asks through the compositor's private connection.** The compositor
draws the question itself.

**On other desktops** (not built now), the portal Access dialog
(`org.freedesktop.impl.portal.Access`) is the closest fit:
- **The choices:** its options can carry the scope (once, the session, a set
  time).
- **No public door yet:** it has no public front end today; only
  xdg-desktop-portal itself calls it. A hub using it must check who owns the
  backend's bus name (that it's the desktop's portal backend), since any
  program could claim the name first and answer "allow". A portal of its own
  would avoid this (decision 7: not now).
- **What it shows:**
  - **The question:** the agent's profile, the provider and tool, and a
    readable summary of the arguments.
  - **The choices:** once, for this session, or for a set time.

The desktop's portal backend draws it. Under Waybox, Waybox's own backend
draws it in the compositor, above everything, where clients can't overlap
or fake it.

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
- input is ignored for about half a second after it appears, so a click
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

The hub publishes its state on D-Bus, at `org.waybox.AgentHub1` on the
session bus (`doc/dbus/org.waybox.AgentHub1.xml`), until a shared name is
agreed (`org.freedesktop.AgentHub1`).

**Properties:**
- `Agents`: connected shims, with profile and self-declared name.
- `Pending`: questions waiting for the user's answer.
- `Providers`: id, name, state (`approved`, `pending`, `refused`), tool count.
- `Grants`: what's granted, to which profile, until when.
- `Active`: what's happening now, such as capture or input in progress, or
  a tool call running.

**Signals:**
- `CallStarted(agent, tool, sensitivity)`
- `CallEnded(agent, tool, result)`
- `ProvidersChanged`
- `GrantsChanged`

**Methods:**
- `Revoke(grant)`
- `RevokeAll()`
- `Stop()`: what the stop key does, from a panel button
- `RefuseProvider(id)`
- `RecentCalls(count)`: the latest calls, metadata only, with `count`
  capped at 100

**Who may call:**
- **Methods:** callable by any process of the user (the session bus already
  limits this to the user).
- **Nothing that grants.** Granting, approving providers and binding
  profiles happen only through the compositor's confirmations. A panel can
  show and take away, never give.
- **Fail-safe abuse only:** a program spamming `Stop` or `RevokeAll` can only
  make things safer. Such calls are rate-limited and logged.

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

The hub appends one JSON line per call to
`$XDG_STATE_HOME/agent-tools/audit.log`. Each line records:
- time;
- agent and profile;
- provider and tool;
- sensitivity;
- arguments, with secrets redacted (from each tool's `Secret` declaration;
  synthetic input into password fields is recorded as its length only);
- outcome: allowed, asked and answered, refused, or failed;
- for `capture`: what was captured;
- for `input`: what was typed.

Surfaces can follow it through `CallEnded`, which carries no arguments.

**The file is the user's alone:** mode 0600, in a directory with mode 0700.
It's on by default. Turning it off counts as raising power, so it needs the
compositor's confirmation.

## The compositor's part

**No Wayland protocol is involved or defined.** Agents, shims, the hub,
providers and surfaces aren't Wayland clients. The hub reaches the
compositor through its private connection: Waybox's IPC protocol (the
same JSON requests `waybox-msg` sends), plus the requests only the hub may
make (consent, confirmations, capture, input, the stop key's news), which
the general IPC socket refuses.
Capture and input are done inside the compositor at the hub's request,
not through the protected capture and virtual input protocols, which stay
closed to sandboxed clients as they are.

A Wayland protocol could make sense one day, if several compositors want
to share the compositor's side: mediated capture with exclusions, and a
trusted consent overlay. That would come after the rest has proved itself,
as a cross-compositor proposal.

A compositor that supports this provides the following.
- **A provider** for its own state and actions: windows, desktops, outputs,
  focus, move and resize. Under Waybox, the hub offers it as the `waybox`
  provider, over the private connection (its tools are below).
- **Consent drawn where clients can't reach it.** Under Waybox, the
  compositor draws questions and confirmations itself, asked over the
  private connection; no portal backend is involved. (A portal Access
  backend is the route on other desktops; see the decisions below.)
- **The hub,** started by the compositor, with the private connection.
- **Capture and input,** mediated:
  - **Capture:** the compositor itself draws what's captured, leaving out
    private windows and the lock screen.
  - **Input:** synthesized only into the granted window.
  - **Requests:** accepted only on the hub's private connection.
  - **Defaults:** grants are for one window; a whole output only by explicit
    choice.
- **Confirmations and questions,** answered by physical input only.
- **The stop key,** always.
- **The indicator,** while capture or input is active, drawn by the
  compositor. Switchable; a surface may show one too.

### The `waybox` provider's tools

| Tool | Sensitivity | What it does |
|---|---|---|
| `list_windows` | read (titles: content) | windows: id, app id, title, desktop, output, geometry, state, focused; each window's process id and whether it answers (Waybox's ping) |
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
| `ui.notify(title, body, actions?)` | a notice, with buttons if given | the button chosen, or nothing |
| `ui.ask(prompt, default?, secret?)` | a one-line question | the text, or cancelled |
| `ui.choose(prompt, items, multiple?)` | a list to pick from | the item(s), or cancelled |
| `ui.form(title, schema)` | a form | the values, or cancelled |
| `ui.output_open(title)`, `ui.output_append(id, text)`, `ui.output_close(id)` | a stream of output (a log, a long answer) | an id |
| `ui.progress(id, fraction?, text)` | progress on a running task | nothing |
| `ui.pick_file(prompt, save?, filters?)` | the file chooser | the path, or cancelled |

`ui.form` takes **MCP elicitation's schema**: a flat object of text,
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
  pick is itself the consent to share the file.

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
  answer within a few seconds is treated as failed. The hub then uses the
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
because those take physical input in a dialog the compositor draws. The
levels confine an agent only as far as the agent is otherwise confined. For
real confinement, run the agent in a sandbox where the shim's socket is the
only way out.

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

## Versioning

**Manifests** carry `Version=1` in `[Provider]`. The D-Bus interface is
versioned by name (`AgentHub1`). MCP versions are negotiated as MCP does.

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
4. **Sandboxed agents.** Out of scope for now: no Agent portal.
5. **Resource streams.** Tools only at first; resources and subscriptions
   later.
6. **The name.** `org.waybox.AgentHub1` and `org.waybox.AgentUI1`, until a
   shared name is agreed.
7. **The consent route on other desktops.** Not now. Waybox draws its own
   questions; a portal of its own is proposed only once this has proved
   itself.
8. **Agent UI as a standard of its own.** Later, likewise.

Also decided in the second pass:
- **Settings** in `agent-tools.ini`, not rc.xml.
- **`ui` at `ask` in "safe"**, once per agent connection.
- **The stop key:** Super+Escape by default, changeable, never unbound.
- **From #196:** AT-SPI (`content`), `check_config` (`read`),
  `write_config`, `run_action` and `run_lua` (`exec`), and the process
  tools for a stuck window (`read`; Kill as `destroy`). Raw IPC is dropped.

## The order it's built in

One pull request each, each useful on its own; tests on private buses
and a headless Waybox, never the user's session.

1. **The hub and the shim, started by Waybox:** the private connection;
   the `waybox` provider's `read` and `content` tools (the process id and
   whether a window answers in its list); the audit log. Every agent gets
   "safe", with `ask` acting as `off` until step 2.
2. **Consent and the stop key:** questions drawn by Waybox (physical input
   only, a pause before input counts, Deny the default); grants per
   connection; the D-Bus state for surfaces; `act` and `destroy` tools at
   `ask`.
3. **Settings, profiles and tokens:** `agent-tools.ini`, the confirmation
   for anything that raises power, wbconf's page.
4. **Config and command tools:** `check_config`, `write_config`,
   `run_command`, `run_action`.
5. **Providers of other programs:** manifests, activated and registering
   providers, approval and pinning.
6. **The accessibility provider** (`atspi`).
7. **Scripts:** `wb.mcp_tool`, `wb.policy("mcp")`, `run_lua`.
8. **Agent UI:** the `ui` tools, Waybox's fallbacks, the `agent-ui` rule.
9. **Capture and input,** with the indicator.

## Still open

- **When to propose this outside Waybox,** and as what: the hub's D-Bus
  state, Agent UI and a consent portal are each candidates. After it has
  been in use for a while.
