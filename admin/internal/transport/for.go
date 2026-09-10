package transport

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/hagg3/ewb-server/admin/internal/profile"
)

// For returns the transport a profile dispatches through.
func For(p profile.Profile) (Transport, error) {
	switch p.Kind {
	case profile.Local:
		return Local{}, nil
	case profile.VPS:
		if p.SSHHost == "" {
			return nil, fmt.Errorf("profile %q: vps kind needs ssh_host", p.Name)
		}
		return SSH{Host: p.SSHHost, ControlPath: controlPath(p.SSHHost)}, nil
	default:
		return nil, fmt.Errorf("profile %q: unknown kind %q", p.Name, p.Kind)
	}
}

// controlPath keeps the multiplex socket under the user's ~/.ssh, one per host,
// so a stale socket for one VPS never blocks another.
func controlPath(host string) string {
	safe := filepath.Base(host) // defensive; a Host alias should have no slashes
	if home, err := os.UserHomeDir(); err == nil {
		return filepath.Join(home, ".ssh", "cm-edenadmin-"+safe)
	}
	return "~/.ssh/cm-edenadmin-" + safe
}
