package panels

import (
	"context"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/parse"
	"github.com/hagg3/ewb-server/admin/internal/profile"
)

// PlayersResult is the Players panel payload. Stage 6.2 is read-only: the list
// and each player's ban classification. The kick / ban / op actions arrive with
// 6.3.
type PlayersResult struct {
	Profile string      `json:"profile"`
	Online  bool        `json:"online"`
	Players []PlayerRow `json:"players"`
	Note    string      `json:"note,omitempty"`
}

// PlayerRow is one connected player plus the classification `ban` would apply to
// their name (plan §3.5: show it before the operator commits).
type PlayerRow struct {
	parse.Player
	NameLooksLikeIP bool `json:"name_looks_like_ip"`
}

// Players lists connected players via `edenctl who`.
func (rt *Runtime) Players(ctx context.Context, p profile.Profile) PlayersResult {
	res := PlayersResult{Profile: p.Name}

	if sup := rt.Supervisor(p); sup != nil && !sup.Status().Running {
		res.Note = "server is not running"
		return res
	}

	out, err := rt.runCtl(ctx, p, eden.Who(p))
	if err != nil {
		res.Note = "could not read `who`: " + err.Error()
		return res
	}
	ps, err := parse.Who(out)
	if err != nil {
		res.Note = err.Error()
		return res
	}
	res.Online = true
	for _, pl := range ps {
		res.Players = append(res.Players, PlayerRow{
			Player:          pl,
			NameLooksLikeIP: looksLikeIP(pl.Name),
		})
	}
	return res
}

// looksLikeIP mirrors the server's ctl_looks_like_ip: only digits, dots and
// colons. A username that matches is ambiguous with an IP ban.
func looksLikeIP(s string) bool {
	if s == "" {
		return false
	}
	for _, r := range s {
		if (r < '0' || r > '9') && r != '.' && r != ':' {
			return false
		}
	}
	return true
}
