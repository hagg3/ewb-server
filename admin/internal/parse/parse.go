// Package parse turns the plain-text replies of the tools edenadmin drives
// (edenctl over the control socket, systemctl / df / journalctl on the host,
// eden_import) into typed values.
//
// Every parser follows the same contract: it takes the raw bytes and returns
// (value, error), never panics, and treats an unrecognised shape as an error
// rather than a zero value. The reply formats are text with no version marker,
// so each parser is pinned by a golden corpus in admin/testdata/golden/ (see
// parse_test.go). CRLF is tolerated everywhere — a reply that crossed an ssh
// PTY can pick one up.
package parse

import (
	"strconv"
	"strings"
)

// lines splits s into lines, dropping a trailing empty line and any \r.
func lines(b []byte) []string {
	s := strings.ReplaceAll(string(b), "\r\n", "\n")
	s = strings.TrimRight(s, "\n")
	if s == "" {
		return nil
	}
	return strings.Split(s, "\n")
}

// atoiComma parses "26,277,560" -> 26277560.
func atoiComma(s string) (int64, error) {
	return strconv.ParseInt(strings.ReplaceAll(strings.TrimSpace(s), ",", ""), 10, 64)
}

// --- who ---------------------------------------------------------------------

// Player is one connected client from `who`.
type Player struct {
	Name  string `json:"name"`
	Type  int    `json:"type"` // character model (the T<n> field)
	IP    string `json:"ip"`
	X     int    `json:"x"`
	Y     int    `json:"y"`
	Z     int    `json:"z"`
	Level int    `json:"level"` // operator level 0..2
}

// Who parses:
//
//	N player(s):
//	  <name> (T<type>) <ip>  @ <x>,<y>,<z>  level <n>
//
// The name may contain spaces, so the row is parsed by anchoring on " (T",
// "  @ " and "  level " rather than splitting on whitespace. A row that does not
// match every anchor is a parse error (a truncated reply lands here).
func Who(b []byte) ([]Player, error) {
	ls := lines(b)
	if len(ls) == 0 {
		return nil, parseErr("who", "empty reply")
	}
	head := ls[0]
	n, ok := cutSuffixInt(head, " player(s):")
	if !ok {
		return nil, parseErr("who", "bad header %q", head)
	}
	players := make([]Player, 0, n)
	for _, row := range ls[1:] {
		if strings.TrimSpace(row) == "" {
			continue
		}
		p, err := parseWhoRow(row)
		if err != nil {
			return nil, err
		}
		players = append(players, p)
	}
	if len(players) != n {
		return players, parseErr("who", "header says %d player(s) but %d row(s) followed (truncated reply?)", n, len(players))
	}
	return players, nil
}

func parseWhoRow(row string) (Player, error) {
	s := strings.TrimPrefix(row, "  ")
	iT := strings.Index(s, " (T")
	if iT < 0 {
		return Player{}, parseErr("who", "row missing ' (T': %q", row)
	}
	name := s[:iT]
	rest := s[iT+3:] // after " (T"
	iClose := strings.Index(rest, ") ")
	if iClose < 0 {
		return Player{}, parseErr("who", "row missing ') ': %q", row)
	}
	typ, err := strconv.Atoi(rest[:iClose])
	if err != nil {
		return Player{}, parseErr("who", "bad character type in %q", row)
	}
	rest = rest[iClose+2:] // after ") "

	iAt := strings.Index(rest, "  @ ")
	if iAt < 0 {
		return Player{}, parseErr("who", "row missing '  @ ': %q", row)
	}
	ip := strings.TrimSpace(rest[:iAt])
	rest = rest[iAt+4:]

	iLvl := strings.LastIndex(rest, "  level ")
	if iLvl < 0 {
		return Player{}, parseErr("who", "row missing '  level ': %q", row)
	}
	coords := strings.Split(rest[:iLvl], ",")
	if len(coords) != 3 {
		return Player{}, parseErr("who", "bad coordinates in %q", row)
	}
	x, e1 := strconv.Atoi(strings.TrimSpace(coords[0]))
	y, e2 := strconv.Atoi(strings.TrimSpace(coords[1]))
	z, e3 := strconv.Atoi(strings.TrimSpace(coords[2]))
	lvl, e4 := strconv.Atoi(strings.TrimSpace(rest[iLvl+len("  level "):]))
	if e1 != nil || e2 != nil || e3 != nil || e4 != nil {
		return Player{}, parseErr("who", "bad number in %q", row)
	}
	return Player{Name: name, Type: typ, IP: ip, X: x, Y: y, Z: z, Level: lvl}, nil
}

// --- banlist ---------------------------------------------------------------

// BanEntry is one row of `banlist`. Kind is "ip" or "name".
type BanEntry struct {
	Token string `json:"token"`
	Kind  string `json:"kind"`
}

// Banlist parses either the literal "ban list is empty" or:
//
//	N ban entr(y/ies):
//	  <token>  (ip)
//	  <token>  (name)
func Banlist(b []byte) ([]BanEntry, error) {
	ls := lines(b)
	if len(ls) == 0 {
		return nil, parseErr("banlist", "empty reply")
	}
	if strings.TrimSpace(ls[0]) == "ban list is empty" {
		return []BanEntry{}, nil
	}
	n, ok := cutSuffixInt(ls[0], " ban entr(y/ies):")
	if !ok {
		return nil, parseErr("banlist", "bad header %q", ls[0])
	}
	out := make([]BanEntry, 0, n)
	for _, row := range ls[1:] {
		if strings.TrimSpace(row) == "" {
			continue
		}
		s := strings.TrimPrefix(row, "  ")
		var kind string
		switch {
		case strings.HasSuffix(s, "  (ip)"):
			kind, s = "ip", strings.TrimSuffix(s, "  (ip)")
		case strings.HasSuffix(s, "  (name)"):
			kind, s = "name", strings.TrimSuffix(s, "  (name)")
		default:
			return out, parseErr("banlist", "row missing '  (ip)' / '  (name)': %q", row)
		}
		out = append(out, BanEntry{Token: s, Kind: kind})
	}
	if len(out) != n {
		return out, parseErr("banlist", "header says %d but %d row(s) followed", n, len(out))
	}
	return out, nil
}

// --- region-stats --------------------------------------------------------

// RegionStats is the REGION service counter block.
type RegionStats struct {
	RequestsServed int64   `json:"requests_served"`
	CellsScanned   int64   `json:"cells_scanned"`
	RecordsEmitted int64   `json:"records_emitted"`
	BytesOut       int64   `json:"bytes_out"`
	TotalMS        int64   `json:"total_ms"`
	MeanMS         float64 `json:"mean_ms"`
}

// RegionStatsParse reads:
//
//	REGION service since start:
//	  requests served : 0
//	  cells scanned   : 0
//	  ...
//
// Unknown keys are ignored (a future counter is not a parse failure); a missing
// known key leaves its field zero.
func RegionStatsParse(b []byte) (RegionStats, error) {
	ls := lines(b)
	if len(ls) == 0 || !strings.HasPrefix(ls[0], "REGION service") {
		return RegionStats{}, parseErr("region-stats", "bad header")
	}
	var rs RegionStats
	for _, row := range ls[1:] {
		k, v, ok := strings.Cut(row, ":")
		if !ok {
			continue
		}
		k = strings.TrimSpace(k)
		v = strings.TrimSpace(v)
		switch k {
		case "requests served":
			rs.RequestsServed, _ = strconv.ParseInt(v, 10, 64)
		case "cells scanned":
			rs.CellsScanned, _ = strconv.ParseInt(v, 10, 64)
		case "records emitted":
			rs.RecordsEmitted, _ = strconv.ParseInt(v, 10, 64)
		case "bytes out":
			rs.BytesOut, _ = strconv.ParseInt(v, 10, 64)
		case "total time":
			rs.TotalMS, _ = strconv.ParseInt(strings.TrimSuffix(v, " ms"), 10, 64)
		case "mean per region":
			rs.MeanMS, _ = strconv.ParseFloat(strings.TrimSuffix(v, " ms"), 64)
		}
	}
	return rs, nil
}

// --- audit ---------------------------------------------------------------

// AuditEvent is one line of the server's audit channel. auditLog() emits:
//
//	[Audit] <UTC ISO-8601> <actor> <what>
//
// e.g. "[Audit] 2026-09-09T14:03:19Z player:hagge (level 1) //set: 512 cell(s)".
// Read back from `journalctl -n` (the plain format, not `-o json`) the line
// still carries the syslog prefix "Sep 09 12:01:15 host edenserver[pid]: "
// ahead of the marker; AuditLine anchors on "[Audit] " and drops anything
// before it. A line that carries the marker but not the three trailing fields
// keeps Raw with the parsed fields blank — the panel shows Raw as a fallback.
type AuditEvent struct {
	Time  string `json:"time,omitempty"`
	Actor string `json:"actor,omitempty"`
	What  string `json:"what,omitempty"`
	Raw   string `json:"raw"` // the "[Audit] …" span, syslog prefix stripped
}

const auditMarker = "[Audit] "

// AuditLine parses one line already known to contain the "[Audit] " marker.
func AuditLine(line string) AuditEvent {
	line = strings.TrimRight(line, "\r")
	i := strings.Index(line, auditMarker)
	if i < 0 {
		return AuditEvent{Raw: strings.TrimSpace(line)}
	}
	span := line[i:]
	ev := AuditEvent{Raw: span}
	if f := strings.SplitN(span[len(auditMarker):], " ", 3); len(f) == 3 {
		ev.Time, ev.Actor, ev.What = f[0], f[1], f[2]
	}
	return ev
}

// AuditLines pulls every "[Audit] " line out of a block of log or journal text,
// in order. Non-audit lines (server chatter, control echoes) are dropped.
func AuditLines(b []byte) []AuditEvent {
	var out []AuditEvent
	for _, l := range lines(b) {
		if strings.Contains(l, auditMarker) {
			out = append(out, AuditLine(l))
		}
	}
	return out
}

// --- ack shapes --------------------------------------------------------

// AckKind classifies a one-shot control reply.
type AckKind string

const (
	AckOK      AckKind = "ok"      // "ok" / "ok: ..."
	AckError   AckKind = "error"   // "error: ..."
	AckUsage   AckKind = "usage"   // "usage: ..."
	AckUnknown AckKind = "unknown" // anything else — shown verbatim
)

// Ack classifies the first line of a reply. The write-action panels use this to
// decide success vs. failure; AckUnknown is displayed verbatim and treated as a
// failure by the caller.
func Ack(b []byte) (AckKind, string) {
	ls := lines(b)
	if len(ls) == 0 {
		return AckUnknown, ""
	}
	first := ls[0]
	switch {
	case first == "ok" || strings.HasPrefix(first, "ok:"):
		return AckOK, first
	case strings.HasPrefix(first, "error:"):
		return AckError, first
	case strings.HasPrefix(first, "usage:"):
		return AckUsage, first
	default:
		return AckUnknown, strings.Join(ls, "\n")
	}
}

// --- shared helpers --------------------------------------------------------

// cutSuffixInt trims suffix from s and parses what is left as an int.
func cutSuffixInt(s, suffix string) (int, bool) {
	t, ok := strings.CutSuffix(strings.TrimSpace(s), suffix)
	if !ok {
		return 0, false
	}
	n, err := strconv.Atoi(strings.TrimSpace(t))
	if err != nil {
		return 0, false
	}
	return n, true
}
