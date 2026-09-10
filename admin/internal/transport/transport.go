package transport

import (
	"context"
	"time"
)

// DefaultTimeout bounds any single command. A GUI must never hang; a probe or a
// verb that has not answered in this long is reported as a failure.
const DefaultTimeout = 20 * time.Second

// Command is what a panel asks the transport to run. Argv is the command and
// its arguments with no shell wrapper — the eden/ builders have already added
// any `sudo -n -u <run_as>` prefix, because that is profile knowledge, not
// transport knowledge.
type Command struct {
	Argv    []string
	Stdin   []byte
	Timeout time.Duration // 0 -> DefaultTimeout
}

func (c Command) timeout() time.Duration {
	if c.Timeout <= 0 {
		return DefaultTimeout
	}
	return c.Timeout
}

// Result is the outcome of running a Command. Display is the exact command
// string, shown verbatim in the UI so every action is auditable and every
// quoting question is answerable by looking at it.
type Result struct {
	Display  string
	Stdout   []byte
	Stderr   []byte
	ExitCode int
}

// Transport runs commands either as local processes (`local` profile, dev on
// the Mac) or over ssh to the VPS (`vps` profile).
type Transport interface {
	// Run executes c and returns its Result. A non-nil error means the command
	// could not be run or timed out; a command that ran and exited non-zero is
	// a nil error with Result.ExitCode set.
	Run(ctx context.Context, c Command) (Result, error)

	// Describe returns the exact command string Run would execute, without
	// running it. Used by the Connection panel and by dry-run previews.
	Describe(c Command) string

	// Kind is "local" or "vps".
	Kind() string
}
