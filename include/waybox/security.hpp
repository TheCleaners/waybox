#ifndef WB_SECURITY_HPP
#define WB_SECURITY_HPP

#include <string_view>

namespace wb {

/*
 * Protocols that sandboxed clients (those a sandbox engine such as Flatpak
 * marked with security-context-v1) don't get: they could watch or control
 * other programs or the whole session, and sandboxed programs are meant to
 * go through portals for these (screen sharing, clipboard, ...).
 */
bool is_privileged_protocol(std::string_view interface_name);

/* Every global Waybox makes, classified: ordinary (for any program),
 * privileged (not for sandboxed ones), or unknown (one added and not yet
 * classified: not for sandboxed ones either, and logged, so it's noticed). */
enum class ProtocolClass { Ordinary, Privileged, Unknown };
ProtocolClass classify_protocol(std::string_view interface_name);

}  // namespace wb

#endif /* WB_SECURITY_HPP */
