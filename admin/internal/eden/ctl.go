// Package eden builds the argv for every external tool edenadmin drives:
// edenctl (the control socket), edenserverctl / systemctl / journalctl (the
// host), and eden_import (.eden world conversion).
//
// A builder returns a []string. It has already added any `sudo -n -u <run_as>`
// prefix a vps profile needs, because that is profile knowledge. The transport
// package takes it from there — unchanged for local, shell-quoted for ssh.
package eden

import "github.com/hagg3/ewb-server/admin/internal/profile"

// ctlPrefix is `edenctl -S <socket>`, wrapped in `sudo -n -u <run_as>` for a
// vps profile because the control socket is 0600 and owned by the service user.
func ctlPrefix(p profile.Profile) []string {
	base := []string{p.EdenctlPath(), "-S", p.SocketPath()}
	if p.Kind == profile.VPS {
		return append([]string{"sudo", "-n", "-u", p.RunAs}, base...)
	}
	return base
}

func ctl(p profile.Profile, verb string, args ...string) []string {
	return append(ctlPrefix(p), append([]string{verb}, args...)...)
}

// Who lists connected players.
func Who(p profile.Profile) []string { return ctl(p, "who") }

// Say broadcasts a chat line. edenctl joins its arguments with ':' to build the
// wire line and the server gives `say` a max_args of 1, so a colon in text is
// fine; a newline is not (rejected upstream in the write-action panel).
func Say(p profile.Profile, text string) []string { return ctl(p, "say", text) }

// Kick disconnects a player with a reason.
func Kick(p profile.Profile, name, reason string) []string { return ctl(p, "kick", name, reason) }

// Ban / Unban take a name or an IP.
func Ban(p profile.Profile, token string) []string   { return ctl(p, "ban", token) }
func Unban(p profile.Profile, token string) []string { return ctl(p, "unban", token) }

// Banlist reads the ban list.
func Banlist(p profile.Profile) []string { return ctl(p, "banlist") }

// Op / Deop set a player's operator level.
func Op(p profile.Profile, name, level string) []string { return ctl(p, "op", name, level) }
func Deop(p profile.Profile, name string) []string      { return ctl(p, "deop", name) }

// Save flushes the world to disk.
func Save(p profile.Profile) []string { return ctl(p, "save") }

// CtlStop is the *graceful* stop: it asks the server to saveWorld() and exit.
// It must always be preferred over `systemctl stop`, which sends an unhandled
// SIGTERM and loses up to one autosave interval of edits.
func CtlStop(p profile.Profile) []string { return ctl(p, "stop") }

// RegionStats reports the spatial index sizes.
func RegionStats(p profile.Profile) []string { return ctl(p, "region-stats") }

// Signs lists the world's signs.
func Signs(p profile.Profile) []string { return ctl(p, "signs") }
