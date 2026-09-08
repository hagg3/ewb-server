// sign_store.h — the `eden_signs.txt` sidecar and the `SIGNQ` → `SIGNP` wire
// format. ROADMAP-SERVER stage 1.5, read-only.
//
// Wire contract, from CAPTURE-FINDINGS.md ("Signs — CHANGED") and plan §1.5:
//
//   client  SIGNQ\n
//   server  SIGNP:server:<x>:<y>:<z>:<a>:<b>:<c>:<text>\n   (a burst, no terminator)
//
// Note the literal `server` in field 2 — the same reserved sender token
// `ACTION:server:0:...` uses, which is why `hardening.h` refuses it as a username.
// There is **no terminator**: the capture shows a burst of `SIGNP` lines and then
// nothing. `LISTP` has an `END`, but that is a different subprotocol — do not
// generalize one to the other.
//
// ⚠️ **`a`, `b`, `c` are unknown fields.** Same shape as VuencEdit's sign sidecar
// (`i32 x,y,z; i32 a,b,c; char text[96]`), where `c` is an unproven
// facing-quadrant hypothesis. We emit whatever the file holds, verbatim, and
// invent no semantics. Single-character sign texts in the capture (`q`, `a`, `s`,
// `d`, `o`, `t`) hint that multi-tile signs are indexed by one of them; that is a
// reason not to model them, not a reason to guess.
//
// Storage is a plaintext sidecar in the same style as `eden_world.model`:
//
//   x:y:z:a:b:c:text        one per line, **text last**
//   # comment               (operator convenience — this file is hand-edited,
//   <blank>                  unlike the machine-written world model)
//
// Text is last and taken verbatim from the 7th field on, so a `:` inside sign text
// is safe as long as the parser splits only the first six fields. That is the one
// property this header exists to guarantee.
//
// **Read-only in Phase 1.** No client→server sign-write opcode has ever been
// captured; whether one exists is an open question in CAPTURE-FINDINGS.md. The
// operator populates the file; an admin command is Phase 3 (§3.2 `signs add|rm`).

#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include "hardening.h"

namespace ewb {

/// Sign text long enough for anything observed (the capture's longest was cut off
/// by a 64-byte logger cap, and VuencEdit's own sidecar field is `char[96]`), and
/// short enough that a full `SIGNP` line stays well inside any receive guard.
constexpr size_t SIGN_TEXT_MAX = 256;

/// Same 24-bit key range `ACTION` and `REGION` validate x/z against.
constexpr int SIGN_COORD_MAX = 0xFFFFFF;
/// Highest legal y. `server_posix.cpp` static_asserts this against SV_WORLD_HEIGHT.
constexpr int SIGN_Y_MAX = 255;

struct Sign {
    int x = 0, y = 0, z = 0;
    int a = 0, b = 0, c = 0;
    std::string text;
};

/// Parse one `x:y:z:a:b:c:text` line.
///
/// Returns false for a line that should be *skipped without complaint* (blank or
/// `#` comment) with `skip = true`, and false with `skip = false` for a malformed
/// line the caller should warn about. The split takes exactly six `:` and treats
/// everything after the sixth as text, so `subway: collab Stations` survives.
inline bool parse_sign_line(const std::string& line, Sign& out, bool& skip) {
    skip = false;

    // Tolerate CRLF files and trailing whitespace.
    size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n')) --end;
    size_t begin = 0;
    while (begin < end && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
    if (begin >= end) { skip = true; return false; }
    if (line[begin] == '#') { skip = true; return false; }

    int fields[6];
    size_t pos = begin;
    for (int i = 0; i < 6; ++i) {
        const size_t colon = line.find(':', pos);
        if (colon == std::string::npos || colon >= end) return false;   // fewer than 7 fields
        const std::string tok = line.substr(pos, colon - pos);
        if (tok.empty()) return false;
        char* endp = nullptr;
        const long v = std::strtol(tok.c_str(), &endp, 10);
        if (endp != tok.c_str() + tok.size()) return false;             // not an integer
        if (v < -2147483648LL || v > 2147483647LL) return false;
        fields[i] = static_cast<int>(v);
        pos = colon + 1;
    }

    if (fields[0] < 0 || fields[0] > SIGN_COORD_MAX) return false;      // x
    if (fields[2] < 0 || fields[2] > SIGN_COORD_MAX) return false;      // z
    if (fields[1] < 0 || fields[1] > SIGN_Y_MAX) return false;          // y

    out.x = fields[0];
    out.y = fields[1];
    out.z = fields[2];
    // a/b/c: emitted verbatim, semantics unknown — see the ⚠️ at the top.
    out.a = fields[3];
    out.b = fields[4];
    out.c = fields[5];
    out.text = sanitize_text(line.substr(pos, end - pos), SIGN_TEXT_MAX);
    return true;
}

/// One wire line, terminating '\n' included. The text is re-sanitised here rather
/// than trusted: this is the only function that writes a `SIGNP` to a socket, and a
/// stray control byte in it would desynchronise the client's line framing.
inline std::string format_signp(const Sign& s) {
    return "SIGNP:server:" + std::to_string(s.x) + ":" + std::to_string(s.y) + ":" +
           std::to_string(s.z) + ":" + std::to_string(s.a) + ":" + std::to_string(s.b) + ":" +
           std::to_string(s.c) + ":" + sanitize_text(s.text, SIGN_TEXT_MAX) + "\n";
}

/// The whole burst as one buffer. Built once at load time and re-sent verbatim to
/// every `SIGNQ` — signs are immutable in Phase 1, so there is nothing to rebuild
/// per request and nothing to lock while formatting.
inline std::string format_sign_burst(const std::vector<Sign>& signs) {
    std::string blob;
    blob.reserve(signs.size() * 48);
    for (const Sign& s : signs) blob += format_signp(s);
    return blob;
}

}  // namespace ewb
