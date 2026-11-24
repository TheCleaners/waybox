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

}  // namespace wb

#endif /* WB_SECURITY_HPP */
