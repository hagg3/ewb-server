package panels

import (
	"context"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/parse"
	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/transport"
)

// Audit panel (roadmap 6.7). The server's audit channel is unconditional on
// stdout — every state-changing control command and every completed edit, with
// a UTC stamp and the actor. `--audit-file` keeps a second, restart-surviving
// copy; when it is set that file is the source, otherwise the panel filters the
// "[Audit] " lines out of the live log (local) or journal (vps).
//
// The flag is off by default, so "no dedicated audit file and nothing in the
// log yet" must read as an explicit note — never a blank panel.

const (
	auditReadCap   = 128 * 1024 // bytes read from the tail of a --audit-file / log
	auditTailLines = 2000       // journal / audit-file lines fetched on a vps
)

// AuditResult is the panel payload. Each poll returns the whole current tail and
// the browser replaces its view: audit volume is bounded by the same cell
// budget the edit path enforces, so there is no need for offset bookkeeping.
type AuditResult struct {
	Profile  string             `json:"profile"`
	Kind     string             `json:"kind"`
	Source   string             `json:"source"`
	Fallback bool               `json:"fallback"` // reading [Audit] out of the log/journal, no dedicated --audit-file
	Enabled  bool               `json:"enabled"`  // a dedicated --audit-file is configured
	Events   []parse.AuditEvent `json:"events"`
	Note     string             `json:"note,omitempty"`
}

// Audit assembles the panel for p.
func (rt *Runtime) Audit(ctx context.Context, p profile.Profile) AuditResult {
	if p.Kind == profile.VPS {
		return rt.auditVPS(ctx, p)
	}
	return rt.auditLocal(p)
}

func (rt *Runtime) auditLocal(p profile.Profile) AuditResult {
	res := AuditResult{Profile: p.Name, Kind: string(p.Kind)}

	if af := flagValue(p.Args, "--audit-file"); af != "" {
		res.Enabled = true
		res.Source = af
		b, missing, err := tailFileBytes(af, auditReadCap)
		switch {
		case err != nil:
			res.Note = "could not read " + af + ": " + err.Error()
		case missing:
			res.Note = "--audit-file " + af + " is configured but not created yet — it appears on the first audited action after the server starts."
		default:
			res.Events = parse.AuditLines(b)
			if len(res.Events) == 0 {
				res.Note = "no audit events recorded yet."
			}
		}
		return res
	}

	res.Fallback = true
	path := LogFilePath(p)
	res.Source = path + " ([Audit] lines)"
	b, missing, err := tailFileBytes(path, auditReadCap)
	switch {
	case err != nil:
		res.Note = "could not read " + path + ": " + err.Error()
	case missing:
		res.Note = path + " — no log yet (the server has not run under edenadmin). Set --audit-file <path> for a dedicated feed that survives a restart."
	default:
		res.Events = parse.AuditLines(b)
		if len(res.Events) == 0 {
			res.Note = "no [Audit] lines in the current log. Add --audit-file <path> for a dedicated, restart-surviving audit feed."
		}
	}
	return res
}

func (rt *Runtime) auditVPS(ctx context.Context, p profile.Profile) AuditResult {
	res := AuditResult{Profile: p.Name, Kind: string(p.Kind)}
	t, err := rt.mkTransport(p)
	if err != nil {
		res.Note = err.Error()
		return res
	}
	ctx, cancel := context.WithTimeout(ctx, 25*time.Second)
	defer cancel()

	if af := rt.auditFileFromConf(ctx, p); af != "" {
		res.Enabled = true
		res.Source = af
		r, err := t.Run(ctx, transport.Command{Argv: eden.AuditFileTail(p, af, auditTailLines)})
		switch {
		case err != nil:
			res.Note = "could not read " + af + ": " + err.Error()
		case transport.SudoDenied(r.Stderr):
			res.Note = "sudo -n -u " + p.RunAs + " tail " + af + " was denied — add the sudoers rule (ops/sudoers.d/edenadmin)"
		case r.ExitCode != 0 && strings.Contains(string(r.Stderr), "No such file"):
			res.Note = "--audit-file " + af + " is configured but not created yet — it appears on the first audited action after a restart."
		case r.ExitCode != 0:
			res.Note = orElse(strings.TrimSpace(string(r.Stderr)), "tail exited "+strconv.Itoa(r.ExitCode))
		default:
			res.Events = parse.AuditLines(r.Stdout)
			if len(res.Events) == 0 {
				res.Note = "no audit events recorded yet."
			}
		}
		return res
	}

	res.Fallback = true
	res.Source = "journalctl -u " + p.ServiceName() + " ([Audit] lines)"
	r, err := t.Run(ctx, transport.Command{Argv: eden.JournalTail(p, auditTailLines)})
	if err != nil {
		res.Note = "could not read the journal: " + err.Error()
		return res
	}
	out := r.Stdout
	if len(out) == 0 {
		out = r.Stderr
	}
	if strings.TrimSpace(string(out)) == "" {
		res.Note = "the journal read back empty — the ssh login user may not be in the `adm` / `systemd-journal` group, or the unit has logged nothing. Set --audit-file <path> for a dedicated feed."
		return res
	}
	res.Events = parse.AuditLines(out)
	if len(res.Events) == 0 {
		res.Note = "no [Audit] lines in the last " + strconv.Itoa(auditTailLines) + " journal lines. Add --audit-file <path> for a dedicated audit feed."
	}
	return res
}

// auditFileFromConf reads the EnvironmentFile and pulls --audit-file out of
// EDEN_EXTRA_ARGS. A missing conf, or a conf without the flag, returns "".
func (rt *Runtime) auditFileFromConf(ctx context.Context, p profile.Profile) string {
	body, _ := rt.readConf(ctx, p)
	if len(body) == 0 {
		return ""
	}
	extra, _ := ParseConf(body).Get(confExtra)
	if extra == "" {
		return ""
	}
	return flagValue(strings.Fields(extra), "--audit-file")
}

// tailFileBytes returns up to maxBytes from the end of path. A missing file is
// (nil, true, nil) — not an error.
func tailFileBytes(path string, maxBytes int64) ([]byte, bool, error) {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, true, nil
		}
		return nil, false, err
	}
	defer f.Close()
	fi, err := f.Stat()
	if err != nil {
		return nil, false, err
	}
	if fi.Size() > maxBytes {
		if _, err := f.Seek(fi.Size()-maxBytes, io.SeekStart); err != nil {
			return nil, false, err
		}
	}
	b, err := io.ReadAll(f)
	if err != nil {
		return nil, false, err
	}
	return b, false, nil
}
