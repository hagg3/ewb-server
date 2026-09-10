// Package panels holds the per-screen logic. Stage 6.1 ships the Connection
// panel; the rest arrive with 6.2+.
package panels

import (
	"context"
	"fmt"
	"os"
	"strings"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/transport"
)

// Probe is one check in a connection test.
type Probe struct {
	Name    string `json:"name"`
	Command string `json:"command,omitempty"` // the exact command run, "" for an in-process check
	OK      bool   `json:"ok"`
	Detail  string `json:"detail,omitempty"`
	Soft    bool   `json:"soft"` // a soft failure does not sink the overall result
}

// ConnResult is a whole connection test.
type ConnResult struct {
	Profile string  `json:"profile"`
	Kind    string  `json:"kind"`
	Probes  []Probe `json:"probes"`
	OK      bool    `json:"ok"`
}

func (r *ConnResult) add(p Probe) { r.Probes = append(r.Probes, p) }

func (r *ConnResult) finish() {
	r.OK = true
	for _, p := range r.Probes {
		if !p.OK && !p.Soft {
			r.OK = false
		}
	}
}

// transportFactory builds the transport a vps profile dispatches through. It is
// a field so tests can substitute a fake and exercise the probe logic without
// an ssh binary or a network.
type transportFactory func(profile.Profile) (transport.Transport, error)

// TestConnection runs every probe for a profile and returns the outcome. It
// never returns an error — a failure is a Probe with OK=false and a Detail the
// operator can act on.
func TestConnection(ctx context.Context, p profile.Profile) ConnResult {
	return testConnection(ctx, p, transport.For)
}

func testConnection(ctx context.Context, p profile.Profile, mkTransport transportFactory) ConnResult {
	res := ConnResult{Profile: p.Name, Kind: string(p.Kind)}
	if err := p.Validate(); err != nil {
		res.add(Probe{Name: "profile is valid", OK: false, Detail: err.Error()})
		res.finish()
		return res
	}
	switch p.Kind {
	case profile.Local:
		testLocal(&res, p)
	case profile.VPS:
		testVPS(ctx, &res, p, mkTransport)
	}
	res.finish()
	return res
}

// --- local: in-process checks only (plan §1: the local transport never shells
// out for information) ---

func testLocal(res *ConnResult, p profile.Profile) {
	res.add(dirProbe("world directory", p.WorldDir, true))
	if p.WorldRoot != "" {
		res.add(dirProbe("world root", p.WorldRoot, true))
	}
	if p.ServerBin != "" {
		res.add(execProbe("edenserver binary", p.ServerBin, true))
	}
	res.add(execProbe("eden_import", p.EdenImportPath(), true))
	res.add(execProbe("edenctl", p.EdenctlPath(), true))

	sock := p.SocketPath()
	fi, err := os.Stat(sock)
	switch {
	case err != nil:
		res.add(Probe{
			Name: "control socket", Soft: true, OK: false,
			Detail: sock + ": not present — the server is stopped, or the socket path is wrong",
		})
	case fi.Mode()&os.ModeSocket == 0:
		res.add(Probe{Name: "control socket", OK: false, Detail: sock + " exists but is not a socket"})
	default:
		res.add(Probe{Name: "control socket", OK: true, Detail: sock})
	}
}

func dirProbe(name, path string, required bool) Probe {
	fi, err := os.Stat(path)
	if err != nil {
		return Probe{Name: name, OK: false, Soft: !required, Detail: path + ": " + reason(err)}
	}
	if !fi.IsDir() {
		return Probe{Name: name, OK: false, Soft: !required, Detail: path + " is not a directory"}
	}
	return Probe{Name: name, OK: true, Detail: path}
}

func execProbe(name, path string, required bool) Probe {
	fi, err := os.Stat(path)
	if err != nil {
		return Probe{Name: name, OK: false, Soft: !required, Detail: path + ": " + reason(err)}
	}
	if fi.IsDir() || fi.Mode()&0o111 == 0 {
		return Probe{Name: name, OK: false, Soft: !required, Detail: path + " is not executable"}
	}
	return Probe{Name: name, OK: true, Detail: path}
}

func reason(err error) string {
	if os.IsNotExist(err) {
		return "does not exist"
	}
	return err.Error()
}

// --- vps: over the ssh transport ---

func testVPS(ctx context.Context, res *ConnResult, p profile.Profile, mkTransport transportFactory) {
	t, err := mkTransport(p)
	if err != nil {
		res.add(Probe{Name: "ssh transport", OK: false, Detail: err.Error()})
		return
	}

	// 1. Reachability.
	r1 := runProbe(ctx, t, []string{"true"})
	res.add(namedProbe("ssh reachable", r1, "the Host alias, the key, or the network"))
	if !r1.ok {
		return // nothing else can work; stop here so the report is not a wall of red
	}

	// 2. The control socket, as the service user. Soft: the server may be
	//    stopped. But a sudo password prompt here is a real config gap.
	sockCmd := []string{"sudo", "-n", "-u", p.RunAs, "test", "-S", p.SocketPath()}
	r2 := runProbe(ctx, t, sockCmd)
	pr2 := namedProbe("control socket (as "+p.RunAs+")", r2, "")
	if !r2.ok {
		if transport.SudoDenied(r2.stderr) {
			pr2.Soft = false
			pr2.Detail = "sudo -n was denied — the sudoers rule for " + p.RunAs + " is missing (see admin/README.md)"
		} else {
			pr2.Soft = true
			pr2.Detail = p.SocketPath() + ": not a socket — the server is stopped, or the path is wrong"
		}
	}
	res.add(pr2)

	// 3. The unit is installed and `sudo -n systemctl` works.
	loadCmd := []string{"sudo", "-n", "systemctl", "show", p.ServiceName(), "-p", "LoadState", "--value"}
	r3 := runProbe(ctx, t, loadCmd)
	pr3 := namedProbe("systemd unit "+p.ServiceName(), r3, "")
	if r3.ok {
		state := strings.TrimSpace(string(r3.stdout))
		if state != "loaded" {
			pr3.OK = false
			pr3.Detail = "LoadState=" + state + " — the unit is not installed (see ops/INSTALL.md)"
		} else {
			pr3.Detail = "loaded"
		}
	} else if transport.SudoDenied(r3.stderr) {
		pr3.Detail = "sudo -n was denied for systemctl — the sudoers rule is missing"
	}
	res.add(pr3)

	// 4. Journal readable. Empty output with exit 0 = the login user is not in
	//    adm / systemd-journal and the journal reads back silently empty.
	r4 := runProbe(ctx, t, eden.JournalTail(p, 1))
	pr4 := namedProbe("journal readable", r4, "")
	pr4.Soft = true
	if r4.ok && len(strings.TrimSpace(string(r4.stdout))) == 0 {
		pr4.OK = false
		pr4.Detail = "journalctl returned nothing — the login user is likely not in the `adm` / `systemd-journal` group"
	} else if r4.ok {
		pr4.Detail = "ok"
	}
	res.add(pr4)

	// 5. eden_import present and runnable as the service user (Phase 5 / the
	//    Worlds panel's import flow).
	r5 := runProbe(ctx, t, eden.ImportHelp(p))
	pr5 := namedProbe("eden_import (as "+p.RunAs+")", r5, "")
	if !r5.ok {
		if transport.SudoDenied(r5.stderr) {
			pr5.Detail = "sudo -n was denied — the sudoers rule does not permit eden_import"
		} else {
			pr5.Detail = p.EdenImportPath() + ": not found or not runnable as " + p.RunAs +
				" — build it on the VPS (see ops/INSTALL.md)"
		}
	} else {
		pr5.Detail = p.EdenImportPath()
	}
	res.add(pr5)
}

type probeRun struct {
	command string
	ok      bool
	stdout  []byte
	stderr  []byte
	err     error
}

func runProbe(ctx context.Context, t transport.Transport, argv []string) probeRun {
	cmd := transport.Command{Argv: argv}
	out := probeRun{command: t.Describe(cmd)}
	r, err := t.Run(ctx, cmd)
	out.stdout, out.stderr = r.Stdout, r.Stderr
	out.err = err
	out.ok = err == nil && r.ExitCode == 0
	return out
}

func namedProbe(name string, r probeRun, blames string) Probe {
	p := Probe{Name: name, Command: r.command, OK: r.ok}
	if r.ok {
		return p
	}
	switch {
	case r.err != nil:
		p.Detail = r.err.Error()
	case len(r.stderr) > 0:
		p.Detail = strings.TrimSpace(string(r.stderr))
	default:
		p.Detail = "command exited non-zero"
	}
	if blames != "" {
		p.Detail = fmt.Sprintf("%s  (check %s)", p.Detail, blames)
	}
	return p
}
