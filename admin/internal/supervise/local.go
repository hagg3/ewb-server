// Package supervise is the plain-process supervisor for a `local` profile.
// There is no systemd on macOS (plan §4.4), so edenadmin starts, tracks and
// stops the dev edenserver itself: fork with Setsid so quitting edenadmin does
// not take the world down, capture stdout+stderr to a log file, and record the
// pid in a pidfile alongside the process's real start time so a stale pidfile
// whose pid now belongs to something else is not mistaken for a live server.
package supervise

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// Supervisor tracks one local edenserver.
type Supervisor struct {
	ServerBin string   // absolute path to edenserver
	Args      []string // flags (port, --name, ...)
	WorkDir   string   // cwd for the child (the world dir)
	LogFile   string   // stdout+stderr are appended here
	PidFile   string   // "<pid>\n<start-signature>\n"

	// EdenctlStop, when set, is the argv of a graceful `edenctl ... stop`. Stop
	// tries it first and only escalates to signals if the process outlives it.
	EdenctlStop []string

	// reap holds the child edenadmin started this session, waited on in a
	// goroutine so an exited or killed process does not linger as a zombie.
	// Nil after an edenadmin restart — the process is then an orphan that init
	// reaps, and liveness falls back to the pidfile + ps probe.
	reap *exec.Cmd
}

// Status is a point-in-time view of the supervised process.
type Status struct {
	Running   bool   `json:"running"`
	PID       int    `json:"pid,omitempty"`
	StartedAt string `json:"started_at,omitempty"` // the process's real start time (ps lstart)
	Detail    string `json:"detail,omitempty"`
}

// Start launches edenserver. It refuses if a live process is already tracked.
func (s *Supervisor) Start(ctx context.Context) (Status, error) {
	if st := s.Status(); st.Running {
		return st, fmt.Errorf("already running (pid %d)", st.PID)
	}
	if !filepath.IsAbs(s.ServerBin) {
		return Status{}, errors.New("server_bin must be an absolute path")
	}
	if err := os.MkdirAll(filepath.Dir(s.LogFile), 0o755); err != nil {
		return Status{}, err
	}
	logf, err := os.OpenFile(s.LogFile, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return Status{}, err
	}
	defer logf.Close()

	cmd := exec.Command(s.ServerBin, s.Args...)
	cmd.Dir = s.WorkDir
	cmd.Stdout = logf
	cmd.Stderr = logf
	cmd.Stdin = nil
	// A new session: the child does not share edenadmin's process group, so
	// Ctrl-C in the edenadmin terminal and edenadmin exiting both leave the
	// world running.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}

	if err := cmd.Start(); err != nil {
		return Status{}, err
	}
	pid := cmd.Process.Pid
	// Reap in the background: without a Wait the process would become a zombie
	// when it exits or is signalled. We still track it by pid, not by this
	// handle, so the rest of the code path is identical after an edenadmin
	// restart when there is no handle.
	s.reap = cmd
	go func() { _ = cmd.Wait() }()

	sig := processSignature(pid)
	if err := s.writePidFile(pid, sig); err != nil {
		return Status{}, fmt.Errorf("started pid %d but could not write %s: %w", pid, s.PidFile, err)
	}
	// Give it a moment to bind; if it died immediately, say so.
	time.Sleep(200 * time.Millisecond)
	st := s.Status()
	if !st.Running {
		st.Detail = "process exited immediately — check " + s.LogFile
		return st, fmt.Errorf("edenserver exited on startup")
	}
	return st, nil
}

// Status reads the pidfile and checks liveness. A pid that is alive but whose
// start signature no longer matches (pid reuse) is reported as not running and
// the stale pidfile is removed.
func (s *Supervisor) Status() Status {
	pid, wantSig, err := s.readPidFile()
	if err != nil {
		return Status{Running: false}
	}
	if !pidAlive(pid) {
		_ = os.Remove(s.PidFile)
		return Status{Running: false, Detail: "not running (stale pidfile cleaned)"}
	}
	gotSig := processSignature(pid)
	if wantSig != "" && gotSig != "" && !signaturesMatch(wantSig, gotSig) {
		_ = os.Remove(s.PidFile)
		return Status{Running: false, Detail: fmt.Sprintf("pid %d is now a different process (pidfile ignored)", pid)}
	}
	return Status{Running: true, PID: pid, StartedAt: startedAt(gotSig)}
}

// Stop asks the server to stop gracefully (edenctl stop, which saves the world),
// then escalates to SIGTERM and finally SIGKILL if it does not exit in time.
func (s *Supervisor) Stop(ctx context.Context) (Status, error) {
	st := s.Status()
	if !st.Running {
		return st, nil
	}
	pid := st.PID

	if len(s.EdenctlStop) > 0 {
		c := exec.CommandContext(ctx, s.EdenctlStop[0], s.EdenctlStop[1:]...)
		_ = c.Run() // best effort; we verify by watching the pid
		if waitGone(pid, 10*time.Second) {
			_ = os.Remove(s.PidFile)
			return Status{Running: false, Detail: "stopped gracefully (world saved)"}, nil
		}
	}

	_ = syscall.Kill(pid, syscall.SIGTERM)
	if waitGone(pid, 5*time.Second) {
		_ = os.Remove(s.PidFile)
		return Status{Running: false, Detail: "stopped (SIGTERM — up to one autosave interval of edits may be lost)"}, nil
	}

	_ = syscall.Kill(pid, syscall.SIGKILL)
	if waitGone(pid, 3*time.Second) {
		_ = os.Remove(s.PidFile)
		return Status{Running: false, Detail: "killed (SIGKILL)"}, nil
	}
	return s.Status(), fmt.Errorf("pid %d did not exit after SIGKILL", pid)
}

// Kill is the unconditional last resort.
func (s *Supervisor) Kill() (Status, error) {
	st := s.Status()
	if !st.Running {
		return st, nil
	}
	_ = syscall.Kill(st.PID, syscall.SIGKILL)
	if waitGone(st.PID, 3*time.Second) {
		_ = os.Remove(s.PidFile)
		return Status{Running: false, Detail: "killed (SIGKILL)"}, nil
	}
	return s.Status(), fmt.Errorf("pid %d survived SIGKILL", st.PID)
}

// --- pidfile --------------------------------------------------------

func (s *Supervisor) writePidFile(pid int, sig string) error {
	if err := os.MkdirAll(filepath.Dir(s.PidFile), 0o755); err != nil {
		return err
	}
	return os.WriteFile(s.PidFile, []byte(fmt.Sprintf("%d\n%s\n", pid, sig)), 0o644)
}

func (s *Supervisor) readPidFile() (pid int, sig string, err error) {
	b, err := os.ReadFile(s.PidFile)
	if err != nil {
		return 0, "", err
	}
	fields := strings.SplitN(strings.TrimRight(string(b), "\n"), "\n", 2)
	pid, err = strconv.Atoi(strings.TrimSpace(fields[0]))
	if err != nil || pid <= 0 {
		return 0, "", errors.New("pidfile: bad pid")
	}
	if len(fields) > 1 {
		sig = strings.TrimSpace(fields[1])
	}
	return pid, sig, nil
}

// --- process probes --------------------------------------------------------

func pidAlive(pid int) bool {
	// Signal 0 does not send anything; it just checks the pid is deliverable.
	err := syscall.Kill(pid, 0)
	return err == nil || errors.Is(err, syscall.EPERM)
}

// processSignature is "<start-time>\t<command>" from ps. Comparing it across an
// edenadmin restart catches the case where the recorded pid has been recycled
// by an unrelated process (plan §4: liveness is pid *and* start time).
func processSignature(pid int) string {
	out, err := exec.Command("ps", "-p", strconv.Itoa(pid), "-o", "lstart=,command=").Output()
	if err != nil {
		return ""
	}
	line := strings.TrimSpace(string(out))
	if line == "" {
		return ""
	}
	// lstart is a fixed 24-char ctime string ("Mon Sep  8 14:02:11 2026").
	if len(line) > 25 {
		return strings.TrimSpace(line[:24]) + "\t" + strings.TrimSpace(line[24:])
	}
	return line
}

func signaturesMatch(want, got string) bool {
	ws := strings.SplitN(want, "\t", 2)
	gs := strings.SplitN(got, "\t", 2)
	if len(ws) < 2 || len(gs) < 2 {
		return want == got
	}
	// Same start time and the command still points at the same binary.
	return ws[0] == gs[0] && filepath.Base(firstField(ws[1])) == filepath.Base(firstField(gs[1]))
}

func startedAt(sig string) string {
	if i := strings.IndexByte(sig, '\t'); i >= 0 {
		return sig[:i]
	}
	return ""
}

func firstField(s string) string {
	if i := strings.IndexByte(strings.TrimSpace(s), ' '); i >= 0 {
		return strings.TrimSpace(s)[:i]
	}
	return strings.TrimSpace(s)
}

func waitGone(pid int, within time.Duration) bool {
	deadline := time.Now().Add(within)
	for time.Now().Before(deadline) {
		if !pidAlive(pid) {
			return true
		}
		time.Sleep(50 * time.Millisecond)
	}
	return !pidAlive(pid)
}
