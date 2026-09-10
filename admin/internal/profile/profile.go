// Package profile is the connection registry: ~/.config/edenadmin/profiles.toml,
// mode 0600, no secrets. A profile names either a local dev server (plain
// process on this Mac) or a VPS reached over ssh. SSH keys stay in ~/.ssh; the
// world password is never stored here (plan §1).
package profile

import (
	"fmt"
	"path/filepath"
	"strings"
)

// Kind selects the transport.
type Kind string

const (
	Local Kind = "local"
	VPS   Kind = "vps"
)

// Profile is one connection target. Every field carries matching `toml:` and
// `json:` tags: the toml codec (toml.go) persists it, the httpd JSON envelope
// exchanges it with the browser, and both use the same snake_case names.
type Profile struct {
	Name string `toml:"-" json:"name"`
	Kind Kind   `toml:"kind" json:"kind"`

	// --- local ---
	WorldDir   string   `toml:"world_dir" json:"world_dir,omitempty"`
	WorldRoot  string   `toml:"world_root" json:"world_root,omitempty"`
	ServerBin  string   `toml:"server_bin" json:"server_bin,omitempty"`
	EdenImport string   `toml:"eden_import" json:"eden_import,omitempty"`
	Edenctl    string   `toml:"edenctl" json:"edenctl,omitempty"` // default: <dir of server_bin>/edenctl
	Socket     string   `toml:"socket" json:"socket,omitempty"`   // default: <world_dir>/edenserver.sock
	BackupDir  string   `toml:"backup_dir" json:"backup_dir,omitempty"`
	LogFile    string   `toml:"log_file" json:"log_file,omitempty"`
	Args       []string `toml:"args" json:"args,omitempty"` // edenserver flags for the plain-process supervisor

	// --- vps ---
	SSHHost       string `toml:"ssh_host" json:"ssh_host,omitempty"` // a Host alias in ~/.ssh/config
	Service       string `toml:"service" json:"service,omitempty"`   // systemd unit name
	RunAs         string `toml:"run_as" json:"run_as,omitempty"`     // the service user; edenctl calls are sudo -n -u this
	EnvFile       string `toml:"env_file" json:"env_file,omitempty"`
	Edenserverctl string `toml:"edenserverctl" json:"edenserverctl,omitempty"`
}

// SocketPath is the control socket to pass to edenctl -S.
func (p Profile) SocketPath() string {
	if p.Socket != "" {
		return p.Socket
	}
	return filepath.Join(p.WorldDir, "edenserver.sock")
}

// EdenctlPath is the edenctl program. For a vps profile this is the remote
// path; for local it defaults next to the server binary.
func (p Profile) EdenctlPath() string {
	if p.Edenctl != "" {
		return p.Edenctl
	}
	if p.ServerBin != "" {
		return filepath.Join(filepath.Dir(p.ServerBin), "edenctl")
	}
	return "edenctl"
}

// EdenImportPath is the eden_import program.
func (p Profile) EdenImportPath() string {
	if p.EdenImport != "" {
		return p.EdenImport
	}
	if p.ServerBin != "" {
		return filepath.Join(filepath.Dir(p.ServerBin), "eden_import")
	}
	return "eden_import"
}

// BackupDirPath is where world backups are written. It defaults to
// <world_dir>/backups for a local profile and /var/lib/edenserver/backups for a
// vps profile (matching edenserverctl's own EDENSERVER_BACKUP_DIR default).
func (p Profile) BackupDirPath() string {
	if p.BackupDir != "" {
		return p.BackupDir
	}
	if p.Kind == VPS {
		return "/var/lib/edenserver/backups"
	}
	if p.WorldDir != "" {
		return filepath.Join(p.WorldDir, "backups")
	}
	return ""
}

// EdenserverctlPath is the edenserverctl wrapper (vps only).
func (p Profile) EdenserverctlPath() string {
	if p.Edenserverctl != "" {
		return p.Edenserverctl
	}
	return "edenserverctl"
}

// ServiceName is the systemd unit, defaulting to "edenserver".
func (p Profile) ServiceName() string {
	if p.Service != "" {
		return p.Service
	}
	return "edenserver"
}

// Validate rejects a profile that would produce a broken or unsafe command.
func (p Profile) Validate() error {
	if p.Name == "" {
		return fmt.Errorf("profile has no name")
	}
	if strings.ContainsAny(p.Name, " \t/[]\"") {
		return fmt.Errorf("profile name %q: no spaces, slashes, brackets or quotes", p.Name)
	}
	switch p.Kind {
	case Local:
		return p.validateLocal()
	case VPS:
		return p.validateVPS()
	default:
		return fmt.Errorf("profile %q: kind must be %q or %q, got %q", p.Name, Local, VPS, p.Kind)
	}
}

func (p Profile) validateLocal() error {
	if p.WorldDir == "" {
		return fmt.Errorf("profile %q: world_dir is required for a local profile", p.Name)
	}
	if !filepath.IsAbs(p.WorldDir) {
		return fmt.Errorf("profile %q: world_dir must be an absolute path", p.Name)
	}
	if p.WorldRoot != "" && !filepath.IsAbs(p.WorldRoot) {
		return fmt.Errorf("profile %q: world_root must be an absolute path", p.Name)
	}
	if p.ServerBin != "" && !filepath.IsAbs(p.ServerBin) {
		return fmt.Errorf("profile %q: server_bin must be an absolute path", p.Name)
	}
	if p.BackupDir != "" && !filepath.IsAbs(p.BackupDir) {
		return fmt.Errorf("profile %q: backup_dir must be an absolute path", p.Name)
	}
	// The control socket path lands in a sockaddr_un, which is 104 bytes on
	// darwin including the NUL. A path at or over that can never bind.
	if s := p.SocketPath(); len(s) >= 104 {
		return fmt.Errorf("profile %q: socket path is %d bytes; the OS limit is 103", p.Name, len(s))
	}
	return nil
}

func (p Profile) validateVPS() error {
	if p.SSHHost == "" {
		return fmt.Errorf("profile %q: ssh_host is required for a vps profile", p.Name)
	}
	if strings.ContainsAny(p.SSHHost, " \t") {
		return fmt.Errorf("profile %q: ssh_host must be a single Host alias, no spaces", p.Name)
	}
	if p.WorldDir == "" || !filepath.IsAbs(p.WorldDir) {
		return fmt.Errorf("profile %q: world_dir must be an absolute remote path", p.Name)
	}
	if p.RunAs == "" {
		return fmt.Errorf("profile %q: run_as is required (the service user edenctl calls sudo -u)", p.Name)
	}
	if s := p.SocketPath(); len(s) >= 104 {
		return fmt.Errorf("profile %q: socket path is %d bytes; the OS limit is 103", p.Name, len(s))
	}
	return nil
}

// Set is the whole registry: the named profiles plus which one is active.
type Set struct {
	Active   string
	Profiles map[string]Profile
}

// ActiveProfile returns the active profile and whether it exists.
func (s Set) ActiveProfile() (Profile, bool) {
	p, ok := s.Profiles[s.Active]
	return p, ok
}

// Validate checks every profile and that Active names one of them (when the set
// is non-empty).
func (s Set) Validate() error {
	for name, p := range s.Profiles {
		p.Name = name
		if err := p.Validate(); err != nil {
			return err
		}
	}
	if len(s.Profiles) > 0 && s.Active != "" {
		if _, ok := s.Profiles[s.Active]; !ok {
			return fmt.Errorf("active profile %q is not defined", s.Active)
		}
	}
	return nil
}
