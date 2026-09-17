// motd_store.h — the `eden_motd.txt` sidecar: a world's welcome message.
//
// An operator-editable file of plain text lines that every joining player is
// shown, right after the built-in `[Server] Welcome, <name>!` line and before
// `CAPS:region` (see docs/protocol.md § Join sequence). It exists so that
// "read the rules / join the Discord" does not have to be recompiled into the
// binary, and so it can be changed on a live server: the control socket's
// `motd reload` re-reads the file without a restart, exactly the way
// `signs reload` re-reads `eden_signs.txt`.
//
// Storage mirrors the other sidecars (`eden_signs.txt`, `eden_spawn.txt`):
// plaintext, LF-terminated, hand-editable, `#` comments and blank lines
// skipped. Absent or empty is normal and silent — a server with no MOTD sends
// nothing extra.
//
// **Multi-line is supported**, one wire line per non-blank file line, capped at
// `MOTD_MAX_LINES`. The wire shape is the same `[Server] <text>` chat line the
// control socket's `say` already broadcasts, so no client needs to learn a new
// message; a client that renders chat renders this.
//
// Each line is put through `sanitize_text()` — the same filter chat and `say`
// use — so a stray control character or an over-long line in a hand-edited file
// cannot inject a second wire line or a fake `SIGNP`. That is the whole reason
// this is a header with a test suite rather than three lines of `getline` in
// the server: the file is operator input, but the result is wire output.

#pragma once

#include <istream>
#include <string>
#include <vector>

#include "hardening.h"   // sanitize_text

namespace ewb {

/// Wire lines one MOTD may produce. A welcome message is read by a player who
/// just spawned into a world; more than this is a wall of text, and an
/// accidentally-`cat`'d file should not become a thousand-line chat flood.
constexpr size_t MOTD_MAX_LINES = 8;

/// Bytes of text per line, matching the server's chat cap (`SV_MAX_CHAT`) so a
/// MOTD line is never larger than a line a player could type.
constexpr size_t MOTD_MAX_LINE = 256;

/// Parse a MOTD sidecar into its text lines (no `[Server] ` prefix, no newline).
///
/// Blank lines and lines whose first non-space character is `#` are skipped, so
/// the file can carry comments and be laid out readably. A line that sanitises
/// down to nothing (all control characters) is skipped too. Lines past
/// `max_lines` are dropped and counted in `*dropped` when that is non-null.
inline std::vector<std::string> parse_motd(std::istream& in,
                                           size_t max_lines = MOTD_MAX_LINES,
                                           size_t max_len   = MOTD_MAX_LINE,
                                           size_t* dropped  = nullptr) {
    std::vector<std::string> out;
    if (dropped) *dropped = 0;
    std::string line;
    while (std::getline(in, line)) {
        size_t end = line.size();
        while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n' ||
                           line[end - 1] == ' '  || line[end - 1] == '\t'))
            --end;
        size_t begin = 0;
        while (begin < end && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
        if (begin >= end) continue;              // blank
        if (line[begin] == '#') continue;        // comment

        std::string text = sanitize_text(line.substr(begin, end - begin), max_len);
        if (text.empty()) continue;              // nothing survived sanitising
        if (out.size() >= max_lines) { if (dropped) ++*dropped; continue; }
        out.push_back(std::move(text));
    }
    return out;
}

/// One MOTD text line as it goes on the wire, terminating '\n' included.
inline std::string format_motd_line(const std::string& text) {
    return "[Server] " + text + "\n";
}

/// The whole MOTD as wire-ready lines, in file order. The server sends these one
/// `sendLine()` at a time rather than as a single blob, so each is an
/// independent queue entry like every other `[Server]` notice.
inline std::vector<std::string> format_motd_burst(const std::vector<std::string>& text_lines) {
    std::vector<std::string> out;
    out.reserve(text_lines.size());
    for (const std::string& t : text_lines) out.push_back(format_motd_line(t));
    return out;
}

}  // namespace ewb
