package supervise

import (
	"context"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

func newSup(t *testing.T, bin string, args ...string) *Supervisor {
	t.Helper()
	dir := t.TempDir()
	return &Supervisor{
		ServerBin: bin,
		Args:      args,
		WorkDir:   dir,
		LogFile:   filepath.Join(dir, "server.log"),
		PidFile:   filepath.Join(dir, "server.pid"),
	}
}

func TestSupervisorLifecycle(t *testing.T) {
	s := newSup(t, "/bin/sleep", "30")

	st, err := s.Start(context.Background())
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	if !st.Running || st.PID == 0 {
		t.Fatalf("after Start: %+v", st)
	}
	if _, err := os.Stat(s.PidFile); err != nil {
		t.Fatalf("pidfile not written: %v", err)
	}

	if !s.Status().Running {
		t.Fatal("Status says not running right after Start")
	}

	st, err = s.Stop(context.Background())
	if err != nil {
		t.Fatalf("Stop: %v", err)
	}
	if st.Running {
		t.Fatalf("still running after Stop: %+v", st)
	}
	if _, err := os.Stat(s.PidFile); !os.IsNotExist(err) {
		t.Errorf("pidfile not cleaned after Stop (err=%v)", err)
	}
}

func TestSupervisorRefusesDoubleStart(t *testing.T) {
	s := newSup(t, "/bin/sleep", "30")
	if _, err := s.Start(context.Background()); err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer s.Kill()
	if _, err := s.Start(context.Background()); err == nil {
		t.Fatal("second Start should have been refused")
	}
}

func TestSupervisorImmediateExit(t *testing.T) {
	s := newSup(t, "/bin/sh", "-c", "exit 3")
	st, err := s.Start(context.Background())
	if err == nil {
		t.Fatal("Start should report a process that exited immediately")
	}
	if st.Running {
		t.Errorf("status = %+v", st)
	}
}

func TestStalePidfileNotClaimed(t *testing.T) {
	s := newSup(t, "/bin/sleep", "30")
	// A pidfile pointing at pid 1 (launchd/init) with our recorded signature.
	// pid 1 is always alive but is not our process, so Status must reject it.
	sig := "Mon Sep  8 14:02:11 2026\t/bin/sleep 30"
	if err := s.writePidFile(1, sig); err != nil {
		t.Fatal(err)
	}
	if st := s.Status(); st.Running {
		t.Fatalf("stale pidfile for pid 1 was claimed as running: %+v", st)
	}
	if _, err := os.Stat(s.PidFile); !os.IsNotExist(err) {
		t.Error("stale pidfile should have been removed")
	}
}

func TestPidfileWithDeadPidCleaned(t *testing.T) {
	s := newSup(t, "/bin/sleep", "30")
	// A pid that is almost certainly not in use.
	if err := s.writePidFile(999999, "x\ty"); err != nil {
		t.Fatal(err)
	}
	if s.Status().Running {
		t.Fatal("dead pid claimed as running")
	}
}

func TestLogTailAppendAndRotate(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "app.log")
	tail := &LogTail{Path: path}

	// Missing file is not an error.
	r, err := tail.Read(0)
	if err != nil || !r.Missing {
		t.Fatalf("missing-file read = %+v, %v", r, err)
	}

	os.WriteFile(path, []byte("line one\nline two\n"), 0o644)
	r, _ = tail.Read(0)
	if r.Text != "line one\nline two\n" {
		t.Fatalf("first read = %q", r.Text)
	}

	// Append: only the new bytes come back.
	f, _ := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0o644)
	f.WriteString("line three\n")
	f.Close()
	r, _ = tail.Read(0)
	if r.Text != "line three\n" {
		t.Fatalf("incremental read = %q", r.Text)
	}

	// Rotation: a smaller file replaces it. The tail restarts from 0.
	os.WriteFile(path, []byte("fresh\n"), 0o644)
	r, _ = tail.Read(0)
	if !r.Rotated || r.Text != "fresh\n" {
		t.Fatalf("post-rotation read = %+v", r)
	}
}

func TestLogTailMaxBytes(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "big.log")
	big := make([]byte, 5000)
	for i := range big {
		big[i] = 'a'
	}
	os.WriteFile(path, big, 0o644)
	tail := &LogTail{Path: path}
	r, _ := tail.Read(1000)
	if len(r.Data) != 1000 {
		t.Fatalf("capped read returned %d bytes, want 1000", len(r.Data))
	}
	if r.Offset != 5000 {
		t.Errorf("offset = %d, want 5000", r.Offset)
	}
}

func TestGracefulStopPreferred(t *testing.T) {
	dir := t.TempDir()
	marker := filepath.Join(dir, "graceful")
	s := newSup(t, "/bin/sleep", "30")
	// A fake "edenctl stop" that records it ran and then kills the process,
	// standing in for the server saving and exiting on the control command.
	pidHolder := filepath.Join(dir, "child.pid")
	s.EdenctlStop = []string{"/bin/sh", "-c",
		"touch " + marker + " && kill $(cat " + pidHolder + ")"}

	st, err := s.Start(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	os.WriteFile(pidHolder, []byte(strconv.Itoa(st.PID)), 0o644)

	st, err = s.Stop(context.Background())
	if err != nil {
		t.Fatalf("Stop: %v", err)
	}
	if st.Running {
		t.Fatal("still running")
	}
	if _, err := os.Stat(marker); err != nil {
		t.Error("graceful edenctl stop path was not taken")
	}
}

func TestWaitGone(t *testing.T) {
	if !waitGone(999999, 100*time.Millisecond) {
		t.Error("waitGone should be true for a dead pid")
	}
}
