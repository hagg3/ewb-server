package panels

import (
	"strings"

	"github.com/hagg3/ewb-server/admin/internal/profile"
)

// LogsResult is one poll of the Logs panel for a local profile. The browser
// keeps the running text and appends Lines; Reset true means it should clear
// first (the file rotated or shrank).
type LogsResult struct {
	Profile string    `json:"profile"`
	Path    string    `json:"path"`
	Lines   []LogLine `json:"lines"`
	Offset  int64     `json:"offset"`
	Reset   bool      `json:"reset"`
	Missing bool      `json:"missing"`
}

// LogLine is one line with the severity tag the UI highlights on. edenserver
// writes everything to stdout at one level, so the tag is derived from the
// "[Server]" / "[Control]" / "[Audit]" prefix, not a syslog priority.
type LogLine struct {
	Text string `json:"text"`
	Tag  string `json:"tag,omitempty"`
}

const logReadCap = 256 * 1024

// Logs reads the bytes appended to the local profile's log file since the last
// poll. fromStart forces a full re-read (the browser asks for this on first
// load or after switching profiles).
func (rt *Runtime) Logs(p profile.Profile, fromStart bool) (LogsResult, error) {
	t := rt.tail(p)
	if fromStart {
		t.Reset()
	}
	r, err := t.Read(logReadCap)
	if err != nil {
		return LogsResult{}, err
	}
	res := LogsResult{
		Profile: p.Name,
		Path:    LogFilePath(p),
		Offset:  r.Offset,
		Reset:   fromStart || r.Rotated,
		Missing: r.Missing,
	}
	for _, line := range strings.Split(strings.TrimRight(r.Text, "\n"), "\n") {
		if line == "" {
			continue
		}
		res.Lines = append(res.Lines, LogLine{Text: line, Tag: logTag(line)})
	}
	return res, nil
}

func logTag(line string) string {
	for _, tag := range []string{"[Audit]", "[Control]", "[Server]"} {
		if strings.Contains(line, tag) {
			return strings.Trim(tag, "[]")
		}
	}
	return ""
}
