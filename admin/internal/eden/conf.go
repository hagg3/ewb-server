package eden

import "github.com/hagg3/ewb-server/admin/internal/profile"

// Config / EnvironmentFile verbs (roadmap 6.6). vps only — a local profile's
// configuration is its `args` in profiles.toml, edited in place by edenadmin
// (no shell). Every builder here quotes cleanly through the ssh transport.

// WriteconfPath is the validating, atomic conf writer installed from
// ops/edenserver-writeconf. It is the only thing the edenadmin sudoers rule lets
// the GUI write to /etc through — never `sudo tee`.
const WriteconfPath = "/usr/local/bin/edenserver-writeconf"

// ConfPath is the systemd EnvironmentFile. A vps profile may override it with
// env_file; otherwise /etc/edenserver.conf, matching ops/edenserver.service.
func ConfPath(p profile.Profile) string {
	if p.EnvFile != "" {
		return p.EnvFile
	}
	return "/etc/edenserver.conf"
}

// ConfRead cats the EnvironmentFile without sudo. The file is 0600 root:root in
// the shipped layout, so this usually fails and the caller retries ConfReadSudo;
// reading plain first keeps a correctly group-readable install from needing a
// sudoers entry it does not have.
func ConfRead(p profile.Profile) []string {
	return []string{"cat", ConfPath(p)}
}

// ConfReadSudo is the fallback: `sudo -n cat <conf>`.
func ConfReadSudo(p profile.Profile) []string {
	return []string{"sudo", "-n", "cat", ConfPath(p)}
}

// ConfWrite streams the proposed file to edenserver-writeconf on stdin. The
// wrapper allowlist-validates every line, checks the required keys, and installs
// it with a temp file + rename(2) at 0600 root:root. The `env EDENSERVER_CONF=`
// form (which the sudoers rule must permit) carries a non-default env_file.
func ConfWrite(p profile.Profile) []string {
	if p.EnvFile != "" {
		return []string{"sudo", "-n", "env", "EDENSERVER_CONF=" + p.EnvFile, WriteconfPath}
	}
	return []string{"sudo", "-n", WriteconfPath}
}
