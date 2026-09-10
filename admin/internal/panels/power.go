package panels

import (
	"context"
	"fmt"
	"strings"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/parse"
	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/transport"
)

// PowerResult reports the outcome of a start/stop/restart/kill together with a
// freshly re-read Status, so the UI never renders an optimistic state.
type PowerResult struct {
	Action  string       `json:"action"`
	OK      bool         `json:"ok"`
	Message string       `json:"message"`
	Status  StatusResult `json:"status"`
}

// PowerActions is the set the /api/power route accepts.
var PowerActions = map[string]bool{"start": true, "stop": true, "restart": true, "kill": true}

// Power runs a lifecycle action. For a local profile it drives the plain-process
// supervisor; for a vps profile "stop" and "restart" go through `edenctl stop`
// (graceful — the server has no SIGTERM handler, so `systemctl stop` would
// discard up to one autosave interval of edits; roadmap 6.3 / plan §5.4). Only
// "kill" is an ungraceful signal, and only "start"/"kill" touch systemctl.
func (rt *Runtime) Power(ctx context.Context, p profile.Profile, action string) PowerResult {
	res := PowerResult{Action: action}
	switch p.Kind {
	case profile.Local:
		res.OK, res.Message = rt.powerLocal(ctx, p, action)
	case profile.VPS:
		res.OK, res.Message = rt.powerVPS(ctx, p, action)
	default:
		res.OK, res.Message = false, "unknown profile kind: "+string(p.Kind)
	}
	res.Status = rt.Status(ctx, p)
	return res
}

func orElse(s, fallback string) string {
	if strings.TrimSpace(s) == "" {
		return fallback
	}
	return s
}

func (rt *Runtime) powerLocal(ctx context.Context, p profile.Profile, action string) (bool, string) {
	sup := rt.Supervisor(p)
	if sup == nil {
		return false, "this profile has no local supervisor"
	}
	switch action {
	case "start":
		st, err := sup.Start(ctx)
		if err != nil {
			return false, err.Error()
		}
		return true, fmt.Sprintf("started (pid %d)", st.PID)
	case "stop":
		st, err := sup.Stop(ctx)
		if err != nil {
			return false, err.Error()
		}
		return true, orElse(st.Detail, "stopped")
	case "restart":
		if _, err := sup.Stop(ctx); err != nil {
			return false, "stop failed, not restarting: " + err.Error()
		}
		st, err := sup.Start(ctx)
		if err != nil {
			return false, "stopped, but start failed: " + err.Error()
		}
		return true, fmt.Sprintf("restarted (pid %d)", st.PID)
	case "kill":
		st, err := sup.Kill()
		if err != nil {
			return false, err.Error()
		}
		return true, orElse(st.Detail, "killed")
	default:
		return false, "unknown action: " + action
	}
}

func (rt *Runtime) powerVPS(ctx context.Context, p profile.Profile, action string) (bool, string) {
	switch action {
	case "start":
		return rt.runHost(ctx, p, eden.Start(p), "start requested")
	case "kill":
		return rt.runHost(ctx, p, eden.Kill(p),
			"SIGKILL sent — edits since the last autosave (up to one interval) are lost")
	case "stop":
		ok, line := rt.gracefulStop(ctx, p)
		if !ok {
			return false, line
		}
		return true, "graceful stop requested (world saved): " + line
	case "restart":
		ok, line := rt.gracefulStop(ctx, p)
		if !ok {
			return false, "not restarting — " + line
		}
		rt.waitVPSInactive(ctx, p, 15*time.Second)
		up, msg := rt.runHost(ctx, p, eden.Start(p), "restarted (graceful stop + start)")
		if !up {
			return false, "graceful stop done but start failed: " + msg
		}
		return true, msg
	default:
		return false, "unknown action: " + action
	}
}

// gracefulStop asks the running server to save and exit via the control socket.
// It never falls back to `systemctl stop` — that is the whole point (plan §5.4).
func (rt *Runtime) gracefulStop(ctx context.Context, p profile.Profile) (bool, string) {
	out, err := rt.runCtl(ctx, p, eden.CtlStop(p))
	if err != nil {
		return false, "could not reach the control socket for a graceful stop: " + err.Error()
	}
	kind, line := parse.Ack(out)
	if kind == parse.AckError || kind == parse.AckUsage || kind == parse.AckUnknown {
		return false, "graceful stop refused: " + line
	}
	return true, line
}

func (rt *Runtime) runHost(ctx context.Context, p profile.Profile, argv []string, okMsg string) (bool, string) {
	t, err := rt.mkTransport(p)
	if err != nil {
		return false, err.Error()
	}
	cctx, cancel := context.WithTimeout(ctx, 25*time.Second)
	defer cancel()
	r, err := t.Run(cctx, transport.Command{Argv: argv})
	if err != nil {
		return false, err.Error()
	}
	if r.ExitCode != 0 {
		if transport.SudoDenied(r.Stderr) {
			return false, "sudo -n was denied — the sudoers rule for systemctl is missing"
		}
		msg := strings.TrimSpace(string(r.Stderr))
		return false, orElse(msg, fmt.Sprintf("command exited %d", r.ExitCode))
	}
	return true, okMsg
}

// waitVPSInactive polls `systemctl show ActiveState` until the unit is inactive
// or the deadline passes, so a restart does not `start` before the old process
// has released the port.
func (rt *Runtime) waitVPSInactive(ctx context.Context, p profile.Profile, within time.Duration) {
	t, err := rt.mkTransport(p)
	if err != nil {
		return
	}
	deadline := time.Now().Add(within)
	for time.Now().Before(deadline) {
		if ctx.Err() != nil {
			return
		}
		cctx, cancel := context.WithTimeout(ctx, 5*time.Second)
		r, err := t.Run(cctx, transport.Command{Argv: eden.SystemctlShow(p, "ActiveState")})
		cancel()
		if err == nil && strings.Contains(string(r.Stdout), "ActiveState=inactive") {
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
}
