package eden

import "github.com/hagg3/ewb-server/admin/internal/profile"

// Host verbs. These only make sense for a vps profile — a local profile has no
// systemd and its start/stop/restart is the plain-process supervisor
// (supervise/), not these. Each is written so the transport's ssh path quotes
// it correctly with no shell metacharacters of our own.

func sudo(args ...string) []string {
	return append([]string{"sudo", "-n"}, args...)
}

// SystemctlShow reads machine-readable unit properties. Several -p flags print
// `Key=Value` lines, which is stable across systemd versions; --value is
// deliberately NOT used (it drops the keys and the mapping becomes positional).
// `systemctl show` is unprivileged, so no sudo.
func SystemctlShow(p profile.Profile, props ...string) []string {
	argv := []string{"systemctl", "show", p.ServiceName()}
	for _, pr := range props {
		argv = append(argv, "-p", pr)
	}
	return argv
}

// StatusProps is the set the Status panel reads.
var StatusProps = []string{
	"ActiveState", "SubState", "MainPID", "ExecMainStartTimestamp",
	"Result", "NRestarts", "LoadState",
}

func Start(p profile.Profile) []string   { return sudo("systemctl", "start", p.ServiceName()) }
func Stop(p profile.Profile) []string    { return sudo("systemctl", "stop", p.ServiceName()) }
func Restart(p profile.Profile) []string { return sudo("systemctl", "restart", p.ServiceName()) }

// Kill sends SIGKILL via systemd (the "Kill" button — last resort).
func Kill(p profile.Profile) []string {
	return sudo("systemctl", "kill", "-s", "SIGKILL", p.ServiceName())
}

// Journal reads the unit's log as JSON. No -f (that never returns). first is
// the initial line count; afterCursor, when set, fetches only newer lines.
func Journal(p profile.Profile, first int, afterCursor string) []string {
	argv := []string{"journalctl", "-u", p.ServiceName(), "--no-pager", "-o", "json"}
	if afterCursor != "" {
		argv = append(argv, "--after-cursor", afterCursor)
	} else {
		argv = append(argv, "-n", itoa(first))
	}
	return argv
}

// JournalTail is the plain non-following tail used by the Connection probe and
// the Audit fallback.
func JournalTail(p profile.Profile, n int) []string {
	return []string{"journalctl", "-u", p.ServiceName(), "-n", itoa(n), "--no-pager"}
}

// AuditFileTail reads the last n lines of a --audit-file on a vps host. The file
// belongs to the service user (the server opens it O_APPEND as itself), so this
// runs `sudo -n -u <run_as> tail`. `--` stops tail from reading a path that
// begins with '-' as an option; the transport shell-quotes the path for ssh.
func AuditFileTail(p profile.Profile, path string, n int) []string {
	return []string{"sudo", "-n", "-u", p.RunAs, "tail", "-n", itoa(n), "--", path}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var buf [20]byte
	i := len(buf)
	for n > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		buf[i] = '-'
	}
	return string(buf[i:])
}
