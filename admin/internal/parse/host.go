package parse

import (
	"encoding/json"
	"strconv"
	"strings"
)

// --- systemctl show --------------------------------------------------------

// SystemctlShow parses the `Key=Value` lines `systemctl show -p ...` prints.
// The value may itself contain '='; only the first '=' splits. A blank value is
// kept (an inactive unit reports ExecMainStartTimestamp= with nothing after).
func SystemctlShow(b []byte) (map[string]string, error) {
	out := map[string]string{}
	for _, row := range lines(b) {
		k, v, ok := strings.Cut(row, "=")
		if !ok {
			return nil, parseErr("systemctl-show", "line without '=': %q", row)
		}
		out[k] = v
	}
	if len(out) == 0 {
		return nil, parseErr("systemctl-show", "no properties")
	}
	return out, nil
}

// Unit is the subset of `systemctl show` the Status panel cares about.
type Unit struct {
	ActiveState    string `json:"active_state"`
	SubState       string `json:"sub_state"`
	MainPID        int    `json:"main_pid"`
	StartTimestamp string `json:"start_timestamp"`
	Result         string `json:"result"`
	NRestarts      int    `json:"n_restarts"`
	LoadState      string `json:"load_state"`
}

// Unit reads the same lines as SystemctlShow and projects the known keys.
func UnitParse(b []byte) (Unit, error) {
	m, err := SystemctlShow(b)
	if err != nil {
		return Unit{}, err
	}
	u := Unit{
		ActiveState:    m["ActiveState"],
		SubState:       m["SubState"],
		StartTimestamp: m["ExecMainStartTimestamp"],
		Result:         m["Result"],
		LoadState:      m["LoadState"],
	}
	u.MainPID, _ = strconv.Atoi(m["MainPID"])
	u.NRestarts, _ = strconv.Atoi(m["NRestarts"])
	return u, nil
}

// --- df -Pk --------------------------------------------------------

// Disk is one filesystem from `df -Pk` (POSIX one-line form, KiB units).
type Disk struct {
	Filesystem  string `json:"filesystem"`
	BlocksKiB   int64  `json:"blocks_kib"`
	UsedKiB     int64  `json:"used_kib"`
	AvailKiB    int64  `json:"avail_kib"`
	CapacityPct int    `json:"capacity_pct"`
	MountedOn   string `json:"mounted_on"`
}

// DfPk parses the last data row of `df -Pk <path>` output. `-P` guarantees one
// logical line per filesystem and `-k` fixes the unit, so there is no
// locale-dependent "1.2G" to deal with.
func DfPk(b []byte) (Disk, error) {
	ls := lines(b)
	if len(ls) < 2 {
		return Disk{}, parseErr("df", "need a header and at least one row")
	}
	f := strings.Fields(ls[len(ls)-1])
	if len(f) < 6 {
		return Disk{}, parseErr("df", "row has %d fields, want >= 6: %q", len(f), ls[len(ls)-1])
	}
	d := Disk{
		Filesystem: f[0],
		MountedOn:  strings.Join(f[5:], " "),
	}
	var errs int
	parse := func(s string) int64 {
		n, err := strconv.ParseInt(s, 10, 64)
		if err != nil {
			errs++
		}
		return n
	}
	d.BlocksKiB = parse(f[1])
	d.UsedKiB = parse(f[2])
	d.AvailKiB = parse(f[3])
	pct, err := strconv.Atoi(strings.TrimSuffix(f[4], "%"))
	if err != nil {
		errs++
	}
	d.CapacityPct = pct
	if errs > 0 {
		return Disk{}, parseErr("df", "non-numeric field in %q", ls[len(ls)-1])
	}
	return d, nil
}

// --- journalctl -o json --------------------------------------------------------

// JournalLine is one entry from `journalctl -o json`. Cursor feeds the next
// poll's --after-cursor so "follow" needs no long-lived process.
type JournalLine struct {
	Cursor     string `json:"cursor"`
	RealtimeUS int64  `json:"realtime_us"`
	Priority   int    `json:"priority"`
	Message    string `json:"message"`
}

// Journal parses the newline-delimited JSON `journalctl -o json` emits. A
// MESSAGE field can be a string or an array of byte values (journald's encoding
// for a message with non-printable bytes); both are handled. A single malformed
// line is skipped rather than failing the whole batch — a truncated final line
// is common when the reply was size-capped.
func Journal(b []byte) ([]JournalLine, error) {
	var out []JournalLine
	for _, row := range lines(b) {
		var raw struct {
			Cursor   string          `json:"__CURSOR"`
			Realtime string          `json:"__REALTIME_TIMESTAMP"`
			Priority string          `json:"PRIORITY"`
			Message  json.RawMessage `json:"MESSAGE"`
		}
		if err := json.Unmarshal([]byte(row), &raw); err != nil {
			continue
		}
		jl := JournalLine{Cursor: raw.Cursor, Message: decodeJournalMessage(raw.Message)}
		jl.RealtimeUS, _ = strconv.ParseInt(raw.Realtime, 10, 64)
		jl.Priority, _ = strconv.Atoi(raw.Priority)
		out = append(out, jl)
	}
	if out == nil {
		return nil, parseErr("journal", "no parseable JSON lines")
	}
	return out, nil
}

func decodeJournalMessage(raw json.RawMessage) string {
	if len(raw) == 0 {
		return ""
	}
	var s string
	if json.Unmarshal(raw, &s) == nil {
		return s
	}
	var bytesArr []byte
	if json.Unmarshal(raw, &bytesArr) == nil {
		return string(bytesArr)
	}
	return string(raw)
}
