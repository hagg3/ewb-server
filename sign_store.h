// sign_store.h — the `eden_signs.txt` sidecar and the `SIGNQ` / `SIGNP` wire
// formats. ROADMAP-SERVER stage 1.5 (read side) and the client sign write.
//
// Wire contract, from CAPTURE-FINDINGS.md ("Signs — CHANGED") and plan §1.5:
//
//   client  SIGNQ\n
//   server  SIGNP:server:<x>:<y>:<z>:<a>:<b>:<c>:<text>\n   (a burst, no terminator)
//
//   client  SIGNP:<x>:<y>:<z>:<a>:<b>:<c>:<text>\n          (placing / editing a sign)
//
// The client's write has **no sender field**; the server's line has `server` there.
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
// **Writers.** The operator (by hand, or `signs add|rm` on the control socket) and,
// since the 2026-09 public launch, players: the retail client sends the write line
// above when a sign is placed (LIVE-FINDINGS 2026-09-08, Session 2). Until the server
// handled it, every player-placed sign was silently discarded.
//
// A sign is also **removed when its block becomes air.** `x:y:z` is the block the sign
// hangs on, not the air cell in front of it, and the client sends nothing when a sign
// goes: removing its block is the only way to remove one in game, and the only line the
// server sees is that block's `ACTION` (LIVE-FINDINGS 2026-09-11). So the server takes
// every sign off a block, whatever its face, whenever any edit stores air there, and
// drops signs on blocks already stored as air when the world loads. Before that, a sign
// whose block was mined stayed in the file and came back when the block was rebuilt.
//
// ⚠️ **A sign's slot is `(x, y, z, a)`, not the block alone.** Real imported worlds
// hold two signs on one block with different `a/b/c`, and across every sidecar on
// hand `a` only ever takes 0–5 — the shape of a block face. So an edit replaces the
// sign in the same block *and* face, and a sign on another face of that block is a
// separate sign. That is inference from data, not a capture; it is the narrowest key
// that keeps both observed collisions apart.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
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

/// The whole burst as one buffer. Rebuilt when the sign list changes (load, an
/// operator edit, a player's write) and re-sent verbatim to every `SIGNQ`, so there
/// is nothing to format per request.
inline std::string format_sign_burst(const std::vector<Sign>& signs) {
    std::string blob;
    blob.reserve(signs.size() * 48);
    for (const Sign& s : signs) blob += format_signp(s);
    return blob;
}

/// One sidecar line, '\n' included — the inverse of `parse_sign_line`.
inline std::string format_sign_file_line(const Sign& s) {
    return std::to_string(s.x) + ":" + std::to_string(s.y) + ":" + std::to_string(s.z) + ":" +
           std::to_string(s.a) + ":" + std::to_string(s.b) + ":" + std::to_string(s.c) + ":" +
           sanitize_text(s.text, SIGN_TEXT_MAX) + "\n";
}

/// Parse a player's sign write: `SIGNP:<x>:<y>:<z>:<a>:<b>:<c>:<text>`, the sidecar
/// grammar behind a `SIGNP:` prefix. A replayed server line (`SIGNP:server:...`)
/// fails the integer parse — nobody gets to write a sign as `server`.
inline bool parse_client_signp(const std::string& line, Sign& out) {
    static const std::string kPrefix = "SIGNP:";
    if (line.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    bool skip = false;
    return parse_sign_line(line.substr(kPrefix.size()), out, skip);
}

/// Same block, same face — see the ⚠️ at the top.
inline bool same_sign_slot(const Sign& p, const Sign& q) {
    return p.x == q.x && p.y == q.y && p.z == q.z && p.a == q.a;
}

enum class SignUpsert { Added, Replaced, Unchanged, Full };

/// Put `s` into its slot: replace the sign already there, or append if the slot is
/// empty and the list is under `max_signs`. A hand-edited sidecar can hold several
/// signs in one slot; the first is replaced and the rest dropped, so the slot
/// converges on one. A client re-sending an identical sign is `Unchanged`, so the
/// caller can skip the rewrite, the relay and the audit line.
inline SignUpsert upsert_sign(std::vector<Sign>& signs, const Sign& s, size_t max_signs) {
    const auto slot = [&](const Sign& cur) { return same_sign_slot(cur, s); };
    auto it = std::find_if(signs.begin(), signs.end(), slot);
    if (it == signs.end()) {
        if (signs.size() >= max_signs) return SignUpsert::Full;
        signs.push_back(s);
        return SignUpsert::Added;
    }
    bool changed = !(it->b == s.b && it->c == s.c && it->text == s.text);
    *it = s;
    const auto dupes = std::remove_if(it + 1, signs.end(), slot);
    if (dupes != signs.end()) {
        signs.erase(dupes, signs.end());
        changed = true;
    }
    return changed ? SignUpsert::Replaced : SignUpsert::Unchanged;
}

/// Drop every sign `is_air(sign)` says has no block under it, keeping the survivors
/// in order. Returns how many were dropped; when `removed` is given they are appended
/// to it, also in order.
///
/// The server's predicate asks whether the block is **stored** as air. A cell the
/// world model doesn't hold is untouched base terrain, which may well be solid, so a
/// sign is never dropped just because its cell is absent.
template <class IsAir>
inline size_t prune_signs(std::vector<Sign>& signs, IsAir is_air,
                          std::vector<Sign>* removed = nullptr) {
    size_t kept = 0;
    for (size_t i = 0; i < signs.size(); ++i) {
        if (is_air(static_cast<const Sign&>(signs[i]))) {
            if (removed) removed->push_back(std::move(signs[i]));
            continue;
        }
        if (kept != i) signs[kept] = std::move(signs[i]);
        ++kept;
    }
    const size_t dropped = signs.size() - kept;
    signs.resize(kept);
    return dropped;
}

/// Every sign on block `(x, y, z)`, whatever its face `a`: what a player loses by
/// removing that block, and what the operator's `signs rm` removes. One definition of
/// "the signs on a block" for both. Returns how many were removed.
inline size_t remove_signs_on_block(std::vector<Sign>& signs, int x, int y, int z,
                                    std::vector<Sign>* removed = nullptr) {
    return prune_signs(signs, [&](const Sign& s) { return s.x == x && s.y == y && s.z == z; },
                       removed);
}

}  // namespace ewb
