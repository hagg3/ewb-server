// spawn_store.h — the `eden_spawn.txt` sidecar: a world's default spawn point.
// ROADMAP-SERVER stage 5.3, read-only.
//
// One line, `x:y:z`, floats in **server** order (x/z horizontal, y height), two
// decimals — the same precision the `SPAWN` unicast and `eden_players.txt` rows
// already carry. `eden_import` writes this file from a `.eden` header's recorded
// player position (`docs/import.md`); the server reads it at startup and hands it
// to a joining player who has no `eden_players.txt` row of their own.
//
// Storage mirrors `eden_signs.txt` / `eden_world.model`: plaintext, hand-editable,
// `#` comments and blank lines skipped. Range is deliberately *not* checked here —
// it is the caller's policy (`eden_import.h` has `eden_spawn_in_range`; the server
// forwards whatever it reads). This header only owns the grammar.

#pragma once

#include <cstdio>
#include <string>

namespace ewb {

struct Spawn {
    float x = 0, y = 0, z = 0;
};

/// One `eden_spawn.txt` line, terminating '\n' included.
inline std::string format_spawn_line(const Spawn& s) {
    char buf[96];
    const int n = std::snprintf(buf, sizeof buf, "%.2f:%.2f:%.2f\n", s.x, s.y, s.z);
    return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

/// Parse one `x:y:z` line into `out`. Leading/trailing whitespace and a trailing
/// CRLF are tolerated; a blank line or a `#` comment reports `skip = true`.
/// Returns false (with `skip = false`) for anything that is not three numbers.
inline bool parse_spawn_line(const std::string& line, Spawn& out, bool& skip) {
    skip = false;

    size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n' ||
                       line[end - 1] == ' ' || line[end - 1] == '\t'))
        --end;
    size_t begin = 0;
    while (begin < end && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
    if (begin >= end) { skip = true; return false; }
    if (line[begin] == '#') { skip = true; return false; }

    const std::string body = line.substr(begin, end - begin);
    float x, y, z;
    char extra;
    // The trailing %c catches a fourth field (`1:2:3:4`) — sscanf would otherwise
    // stop happily after three conversions and accept the junk.
    if (std::sscanf(body.c_str(), "%f:%f:%f%c", &x, &y, &z, &extra) != 3) return false;

    out.x = x;
    out.y = y;
    out.z = z;
    return true;
}

/// Convenience overload for callers that do not care to distinguish "skip" from
/// "malformed" (both mean "no spawn from this line").
inline bool parse_spawn_line(const std::string& line, Spawn& out) {
    bool skip = false;
    return parse_spawn_line(line, out, skip);
}

}  // namespace ewb
