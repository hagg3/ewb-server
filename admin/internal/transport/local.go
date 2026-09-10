package transport

import (
	"bytes"
	"context"
	"errors"
	"os/exec"
	"time"
)

// Local runs commands as plain child processes on this machine. Argv goes
// straight to execve — there is no shell, so nothing in Argv is ever
// interpreted, and Shq is not involved on this path at all.
type Local struct{}

func (Local) Kind() string { return "local" }

func (Local) Describe(c Command) string {
	// For display only. The real execution never builds or parses this string.
	return ShqJoin(c.Argv)
}

func (l Local) Run(ctx context.Context, c Command) (Result, error) {
	if len(c.Argv) == 0 {
		return Result{}, errors.New("transport: empty argv")
	}
	ctx, cancel := context.WithTimeout(ctx, c.timeout())
	defer cancel()

	cmd := exec.CommandContext(ctx, c.Argv[0], c.Argv[1:]...)
	if c.Stdin != nil {
		cmd.Stdin = bytes.NewReader(c.Stdin)
	}
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	err := cmd.Run()
	res := Result{
		Display: l.Describe(c),
		Stdout:  stdout.Bytes(),
		Stderr:  stderr.Bytes(),
	}
	if ctx.Err() == context.DeadlineExceeded {
		return res, &TimeoutError{After: c.timeout()}
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		res.ExitCode = exitErr.ExitCode()
		return res, nil
	}
	if err != nil {
		// Binary missing, permission denied on the file itself, etc.
		return res, classifyExec(err)
	}
	return res, nil
}

// TimeoutError is returned when a command exceeds its deadline.
type TimeoutError struct{ After time.Duration }

func (e *TimeoutError) Error() string { return "command timed out after " + e.After.String() }
