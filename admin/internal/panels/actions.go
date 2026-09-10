package panels

import (
	"context"
	"strconv"
	"strings"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/parse"
	"github.com/hagg3/ewb-server/admin/internal/profile"
)

// PlayerActionReq is one write action from the Players or Bans panel.
type PlayerActionReq struct {
	Verb   string `json:"verb"`             // say | kick | op | deop | ban | unban
	Name   string `json:"name,omitempty"`   // player name, or the ban token for ban/unban
	Reason string `json:"reason,omitempty"` // kick reason (free text)
	Text   string `json:"text,omitempty"`   // say message (free text)
	Level  int    `json:"level,omitempty"`  // op level 0..2
}

// ActionResult is the outcome of one write action. There is no optimistic UI:
// the caller re-polls the owning panel and shows Message, which is the server's
// own reply line verbatim.
type ActionResult struct {
	OK      bool   `json:"ok"`
	Kind    string `json:"kind"`    // parse.AckKind ("ok"/"error"/"usage"/"unknown"), or "rejected"
	Message string `json:"message"` // the reply line, or the rejection reason
}

func rejected(msg string) ActionResult {
	return ActionResult{OK: false, Kind: "rejected", Message: msg}
}

// hasControlChar reports whether s contains a newline or carriage return. The
// control reader is \n-framed and the server's sanitize_text runs *after*
// framing, so a newline smuggled into a free-text argument (`say` text, a kick
// reason) would make its tail a second, unintended control verb. Reject before
// dispatch — this is the same class of bug the transport's shell quoting closes,
// one layer up.
func hasControlChar(s string) bool { return strings.ContainsAny(s, "\n\r") }

// PlayerAction validates and dispatches a single write verb, then classifies the
// reply with parse.Ack. It refuses to send anything when the local server is not
// running (the control socket would not answer) and rejects a free-text argument
// that carries an embedded newline.
func (rt *Runtime) PlayerAction(ctx context.Context, p profile.Profile, a PlayerActionReq) ActionResult {
	name := strings.TrimSpace(a.Name)

	var argv []string
	switch a.Verb {
	case "say":
		if strings.TrimSpace(a.Text) == "" {
			return rejected("say: empty message")
		}
		if hasControlChar(a.Text) {
			return rejected("say: message may not contain a newline")
		}
		argv = eden.Say(p, a.Text)
	case "kick":
		if name == "" {
			return rejected("kick: no player named")
		}
		if hasControlChar(name) || hasControlChar(a.Reason) {
			return rejected("kick: name/reason may not contain a newline")
		}
		reason := a.Reason
		if strings.TrimSpace(reason) == "" {
			reason = "kicked by the operator"
		}
		argv = eden.Kick(p, name, reason)
	case "op":
		if name == "" || hasControlChar(name) {
			return rejected("op: bad player name")
		}
		if a.Level < 0 || a.Level > 2 {
			return rejected("op: level must be 0..2")
		}
		argv = eden.Op(p, name, strconv.Itoa(a.Level))
	case "deop":
		if name == "" || hasControlChar(name) {
			return rejected("deop: bad player name")
		}
		argv = eden.Deop(p, name)
	case "ban":
		if name == "" || hasControlChar(name) {
			return rejected("ban: bad token")
		}
		argv = eden.Ban(p, name)
	case "unban":
		if name == "" || hasControlChar(name) {
			return rejected("unban: bad token")
		}
		argv = eden.Unban(p, name)
	default:
		return rejected("unknown action: " + a.Verb)
	}

	if sup := rt.Supervisor(p); sup != nil && !sup.Status().Running {
		return rejected("server is not running")
	}

	out, err := rt.runCtl(ctx, p, argv)
	if err != nil {
		return ActionResult{OK: false, Kind: "error", Message: "could not reach the control socket: " + err.Error()}
	}
	kind, line := parse.Ack(out)
	return ActionResult{OK: kind == parse.AckOK, Kind: string(kind), Message: line}
}

// BansResult is the Bans panel payload.
type BansResult struct {
	Profile string           `json:"profile"`
	Online  bool             `json:"online"`
	Entries []parse.BanEntry `json:"entries"`
	Note    string           `json:"note,omitempty"`
}

// Bans reads the ban list via `edenctl banlist`.
func (rt *Runtime) Bans(ctx context.Context, p profile.Profile) BansResult {
	res := BansResult{Profile: p.Name, Entries: []parse.BanEntry{}}
	if sup := rt.Supervisor(p); sup != nil && !sup.Status().Running {
		res.Note = "server is not running"
		return res
	}
	out, err := rt.runCtl(ctx, p, eden.Banlist(p))
	if err != nil {
		res.Note = "could not read `banlist`: " + err.Error()
		return res
	}
	entries, err := parse.Banlist(out)
	if err != nil {
		res.Note = err.Error()
		return res
	}
	res.Online = true
	if entries != nil {
		res.Entries = entries
	}
	return res
}
