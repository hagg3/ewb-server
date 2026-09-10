package transport

import (
	"errors"
	"os/exec"
	"strings"
)

// CauseError wraps a raw exec/ssh failure with an operator-legible cause. The
// UI shows Cause; Unwrap keeps the original for logging.
type CauseError struct {
	Cause string
	err   error
}

func (e *CauseError) Error() string { return e.Cause }
func (e *CauseError) Unwrap() error { return e.err }

func classifyExec(err error) error {
	if err == nil {
		return nil
	}
	if errors.Is(err, exec.ErrNotFound) {
		return &CauseError{Cause: "command not found on PATH", err: err}
	}
	msg := err.Error()
	switch {
	case strings.Contains(msg, "no such file or directory"):
		return &CauseError{Cause: "the named binary does not exist", err: err}
	case strings.Contains(msg, "permission denied"):
		return &CauseError{Cause: "the named binary is not executable", err: err}
	default:
		return &CauseError{Cause: msg, err: err}
	}
}

// classifySSH turns ssh's exit-255 stderr into a cause the Connection panel can
// show. Each of the standard failures reads differently and the operator needs
// to know which one it is.
func classifySSH(stderr []byte) error {
	s := string(stderr)
	low := strings.ToLower(s)
	switch {
	case strings.Contains(low, "permission denied"):
		return &CauseError{Cause: "ssh rejected the key — check the Host alias and that the key is loaded", err: errSSH}
	case strings.Contains(low, "could not resolve hostname"):
		return &CauseError{Cause: "ssh could not resolve the host — check the Host alias in ~/.ssh/config", err: errSSH}
	case strings.Contains(low, "connection timed out"), strings.Contains(low, "operation timed out"):
		return &CauseError{Cause: "ssh connection timed out — host down or unreachable", err: errSSH}
	case strings.Contains(low, "connection refused"):
		return &CauseError{Cause: "ssh connection refused — sshd not listening on that port", err: errSSH}
	case strings.Contains(low, "host key verification failed"):
		return &CauseError{Cause: "ssh host key verification failed — the host key changed or is unknown", err: errSSH}
	default:
		t := strings.TrimSpace(s)
		if t == "" {
			t = "ssh failed (exit 255) with no message"
		}
		return &CauseError{Cause: t, err: errSSH}
	}
}

var errSSH = errors.New("ssh transport failure")

// SudoDenied reports whether stderr from a `sudo -n` command is the
// "a password is required" refusal — i.e. a missing sudoers rule. The Connection
// panel calls this out specifically because the fix is a config file, not a
// retry.
func SudoDenied(stderr []byte) bool {
	low := strings.ToLower(string(stderr))
	return strings.Contains(low, "a password is required") ||
		strings.Contains(low, "sudo: a terminal is required") ||
		strings.Contains(low, "no tty present")
}
