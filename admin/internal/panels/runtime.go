package panels

import (
	"context"
	"path/filepath"
	"sync"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/supervise"
	"github.com/hagg3/ewb-server/admin/internal/transport"
)

// Runtime holds the per-profile state the read-only panels need between polls:
// the plain-process supervisor for a local profile, and the resumable log tail.
// It is safe for concurrent use — the HTTP handlers call in on their own
// goroutines.
type Runtime struct {
	mu    sync.Mutex
	sups  map[string]*supervise.Supervisor
	tails map[string]*supervise.LogTail

	// mkTransport is a seam for tests.
	mkTransport transportFactory
}

// NewRuntime builds an empty Runtime wired to the real transport factory.
func NewRuntime() *Runtime {
	return &Runtime{
		sups:        map[string]*supervise.Supervisor{},
		tails:       map[string]*supervise.LogTail{},
		mkTransport: transport.For,
	}
}

// LogFilePath is where a local profile's supervised server writes its output.
func LogFilePath(p profile.Profile) string {
	if p.LogFile != "" {
		return p.LogFile
	}
	return filepath.Join(p.WorldDir, "edenserver.log")
}

func pidFilePath(p profile.Profile) string {
	return filepath.Join(p.WorldDir, "edenadmin-supervisor.pid")
}

// Supervisor returns the supervisor for a local profile, creating it on first
// use. It returns nil for a vps profile (systemd, not this).
func (rt *Runtime) Supervisor(p profile.Profile) *supervise.Supervisor {
	if p.Kind != profile.Local {
		return nil
	}
	rt.mu.Lock()
	defer rt.mu.Unlock()
	s := rt.sups[p.Name]
	if s == nil {
		s = &supervise.Supervisor{
			ServerBin:   p.ServerBin,
			Args:        p.Args,
			WorkDir:     p.WorldDir,
			LogFile:     LogFilePath(p),
			PidFile:     pidFilePath(p),
			EdenctlStop: eden.CtlStop(p),
		}
		rt.sups[p.Name] = s
	} else {
		// Pick up profile edits without dropping the tracked pid.
		s.Args = p.Args
		s.ServerBin = p.ServerBin
	}
	return s
}

func (rt *Runtime) tail(p profile.Profile) *supervise.LogTail {
	rt.mu.Lock()
	defer rt.mu.Unlock()
	t := rt.tails[p.Name]
	if t == nil {
		t = &supervise.LogTail{Path: LogFilePath(p)}
		rt.tails[p.Name] = t
	}
	return t
}

// runCtl dispatches an edenctl argv through the profile's transport and returns
// the combined reply text. A non-zero exit still returns the reply — edenctl
// prints its error to stdout.
func (rt *Runtime) runCtl(ctx context.Context, p profile.Profile, argv []string) ([]byte, error) {
	t, err := rt.mkTransport(p)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	res, err := t.Run(ctx, transport.Command{Argv: argv})
	if err != nil {
		return nil, err
	}
	out := res.Stdout
	if len(out) == 0 {
		out = res.Stderr
	}
	return out, nil
}
