package transport

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/profile"
)

func TestForSelectsTransport(t *testing.T) {
	l, err := For(profile.Profile{Name: "d", Kind: profile.Local})
	if err != nil || l.Kind() != "local" {
		t.Fatalf("local: %v / %v", l, err)
	}
	v, err := For(profile.Profile{Name: "p", Kind: profile.VPS, SSHHost: "eden-vps"})
	if err != nil || v.Kind() != "vps" {
		t.Fatalf("vps: %v / %v", v, err)
	}
	if _, err := For(profile.Profile{Name: "p", Kind: profile.VPS}); err == nil {
		t.Fatal("vps with no ssh_host should error")
	}
}

func TestLocalRun(t *testing.T) {
	l := Local{}
	r, err := l.Run(context.Background(), Command{Argv: []string{"printf", "%s", "hi"}})
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	if string(r.Stdout) != "hi" || r.ExitCode != 0 {
		t.Fatalf("got stdout=%q exit=%d", r.Stdout, r.ExitCode)
	}
}

func TestLocalRunNonZero(t *testing.T) {
	l := Local{}
	r, err := l.Run(context.Background(), Command{Argv: []string{"sh", "-c", "exit 3"}})
	if err != nil {
		t.Fatalf("a command that exits non-zero is not a transport error: %v", err)
	}
	if r.ExitCode != 3 {
		t.Fatalf("ExitCode = %d, want 3", r.ExitCode)
	}
}

func TestLocalRunMissingBinary(t *testing.T) {
	l := Local{}
	_, err := l.Run(context.Background(), Command{Argv: []string{"definitely-not-a-real-binary-xyz"}})
	if err == nil {
		t.Fatal("expected an error for a missing binary")
	}
	var ce *CauseError
	if !errors.As(err, &ce) {
		t.Fatalf("want *CauseError, got %T: %v", err, err)
	}
}

func TestLocalRunStdin(t *testing.T) {
	l := Local{}
	r, err := l.Run(context.Background(), Command{Argv: []string{"cat"}, Stdin: []byte("piped")})
	if err != nil {
		t.Fatal(err)
	}
	if string(r.Stdout) != "piped" {
		t.Fatalf("stdin not delivered: %q", r.Stdout)
	}
}

func TestLocalRunTimeout(t *testing.T) {
	l := Local{}
	_, err := l.Run(context.Background(), Command{Argv: []string{"sleep", "5"}, Timeout: 50 * time.Millisecond})
	var te *TimeoutError
	if !errors.As(err, &te) {
		t.Fatalf("want *TimeoutError, got %T: %v", err, err)
	}
}

func TestLocalDescribeIsDisplayOnly(t *testing.T) {
	got := Local{}.Describe(Command{Argv: []string{"edenctl", "-S", "/w/s.sock", "say", "hi there"}})
	if got != "edenctl -S /w/s.sock say 'hi there'" {
		t.Fatalf("Describe = %q", got)
	}
}

func TestSSHDescribe(t *testing.T) {
	s := SSH{Host: "eden-vps", ControlPath: "~/.ssh/cm"}
	got := s.Describe(Command{Argv: []string{"sudo", "-n", "-u", "edenserver", "edenctl", "-S", "/var/lib/edenserver/world/edenserver.sock", "say", "back in 5"}})
	// The ssh options, the host, the -- separator, then one quoted remote string.
	for _, want := range []string{
		"ssh -o BatchMode=yes",
		"-o ConnectTimeout=5",
		"-o ControlMaster=auto",
		"-o ControlPath=~/.ssh/cm",
		"-o ControlPersist=120",
		"eden-vps --",
		`'sudo -n -u edenserver edenctl -S /var/lib/edenserver/world/edenserver.sock say '\''back in 5'\'''`,
	} {
		if !strings.Contains(got, want) {
			t.Errorf("Describe missing %q\n  in: %s", want, got)
		}
	}
}

func TestSSHRunNeedsHost(t *testing.T) {
	_, err := SSH{}.Run(context.Background(), Command{Argv: []string{"true"}})
	if err == nil {
		t.Fatal("expected an error with no host")
	}
}

func TestSudoDenied(t *testing.T) {
	yes := [][]byte{
		[]byte("sudo: a password is required"),
		[]byte("sudo: a terminal is required to read the password"),
	}
	for _, s := range yes {
		if !SudoDenied(s) {
			t.Errorf("SudoDenied(%q) = false", s)
		}
	}
	if SudoDenied([]byte("edenctl: no such world")) {
		t.Error("SudoDenied false positive")
	}
}
