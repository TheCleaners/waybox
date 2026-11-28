waybox-location-agent(1)

# NAME

waybox-location-agent - ask before an app learns where you are

# SYNOPSIS

*waybox-location-agent* [*--policy* _ask_|_allow_|_deny_] [*--note-kept*] [*--ask-with-notifications*]

# DESCRIPTION

GeoClue tells an app where you are only if its configuration lists the app
or an agent running in your session approves. *waybox-location-agent* is
that agent: when an app asks, it shows a card naming the app, how near it
would learn where you are (a country, a city, a neighborhood, a street,
exactly), a map of that around your time zone's city (drawn offline:
nothing is looked up before you answer), *Deny* and *Allow*, and how long
the answer holds: just this time, for this session, or always. *Escape*
denies; *Return* decides nothing.

Answers kept always go in the xdg permission store's *location* table,
where xdg-desktop-portal keeps its own, so the two agree; a remembered
answer is given at once, without a card. wbconf lists and forgets them.

It registers with GeoClue as *org.waybox.LocationAgent*, or, where
GeoClue's configuration doesn't list that, as *geoclue-demo-agent*, which
distributions' configurations list. Waybox's default autostart starts it
where GeoClue is installed, and then doesn't start GeoClue's demo agent.

# OPTIONS

*--policy* _ask_|_allow_|_deny_
	For apps with no answer remembered: ask (the default), or allow or
	deny them all without asking. A remembered answer still holds.

*--note-kept*
	A notification each time an answer is given without asking (a
	remembered one, or the policy's).

*--ask-with-notifications*
	Ask through the notification daemon (*Deny* and *Allow*, the answer
	holding this time only; dismissed, it's *Deny*) where the daemon shows
	notifications' actions; elsewhere, the card.

# SEE ALSO

*waybox*(1), *wbconf*, *waybox-integration*(5)
