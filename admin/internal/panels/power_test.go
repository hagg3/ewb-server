package panels

import (
	"context"
	"strings"
	"testing"

	"github.com/hagg3/ewb-server/admin/internal/profile"
)

// sleeperProfile is a local profile whose "server" is `/bin/sleep 60` — enough
// to exercise the supervisor lifecycle without a real edenserver.
func sleeperProfile(t *testing.T) profile.Profile {
	t.Helper()
	return profile.Profile{
		Name:      "dev",
		Kind:      profile.Local,
		WorldDir:  t.TempDir(),
		ServerBin: "/bin/sleep",
		Args:      []string{"60"},
		Socket:    "/tmp/edenadmin-power-test.sock",
	}
}

func TestPowerLocalStartThenKill(t *testing.T) {
	p := sleeperProfile(t)
	rt := NewRuntime()

	res := rt.Power(context.Background(), p, "start")
	if !res.OK || !res.Status.Running {
		t.Fatalf("start: %+v", res)
	}
	// A second start is refused, and the running process is untouched.
	if r2 := rt.Power(context.Background(), p, "start"); r2.OK {
		t.Errorf("second start should fail: %+v", r2)
	}

	res = rt.Power(context.Background(), p, "kill")
	if !res.OK || res.Status.Running {
		t.Fatalf("kill: %+v", res)
	}
}

func TestPowerLocalUnknownAction(t *testing.T) {
	p := sleeperProfile(t)
	rt := NewRuntime()
	if res := rt.Power(context.Background(), p, "nuke"); res.OK || !strings.Contains(res.Message, "unknown action") {
		t.Fatalf("unknown action: %+v", res)
	}
}

// TestPowerVPSStopIsGraceful is the roadmap 6.3 exit criterion: a "stop" on a
// vps profile must route through `edenctl stop` and must never invoke
// `systemctl stop` / `systemctl kill` (the server has no SIGTERM handler).
func TestPowerVPSStopIsGraceful(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "edenctl", stdout: "ok: stopping\n"},
	}}
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)

	res := rt.Power(context.Background(), vpsProf, "stop")
	if !res.OK || !strings.Contains(res.Message, "world saved") {
		t.Fatalf("graceful stop should succeed: %+v", res)
	}
	assertNoSystemctlStop(t, f.calls)
	sawEdenctlStop := false
	for _, c := range f.calls {
		if strings.Contains(c, "edenctl") && strings.HasSuffix(strings.TrimSpace(c), "stop") {
			sawEdenctlStop = true
		}
	}
	if !sawEdenctlStop {
		t.Fatalf("expected an `edenctl ... stop`, calls: %v", f.calls)
	}
}

func TestPowerVPSRestartIsGracefulThenStart(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "edenctl", stdout: "ok: stopping\n"},
		{match: "ActiveState", stdout: "ActiveState=inactive\n"},
		{match: "systemctl start", exit: 0},
	}}
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)

	res := rt.Power(context.Background(), vpsProf, "restart")
	if !res.OK {
		t.Fatalf("restart: %+v (calls %v)", res, f.calls)
	}
	assertNoSystemctlStop(t, f.calls)
	sawStart := false
	for _, c := range f.calls {
		if strings.Contains(c, "systemctl start") {
			sawStart = true
		}
	}
	if !sawStart {
		t.Fatalf("restart must end with `systemctl start`, calls: %v", f.calls)
	}
}

func TestPowerVPSStopRefusedWhenSocketDown(t *testing.T) {
	f := &fakeTransport{} // no reply matches -> exit 127, empty output -> Ack unknown
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)

	res := rt.Power(context.Background(), vpsProf, "stop")
	if res.OK {
		t.Fatalf("stop should not report success when the control socket does not answer: %+v", res)
	}
	assertNoSystemctlStop(t, f.calls)
}

func assertNoSystemctlStop(t *testing.T, calls []string) {
	t.Helper()
	for _, c := range calls {
		if strings.Contains(c, "systemctl stop") || strings.Contains(c, "systemctl kill") ||
			strings.Contains(c, "systemctl restart") {
			t.Fatalf("a graceful action shelled out to systemd: %q", c)
		}
	}
}
