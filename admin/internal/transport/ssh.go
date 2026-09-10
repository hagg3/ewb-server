package transport

import (
	"bytes"
	"context"
	"errors"
	"os/exec"
	"strings"
)

// SSH runs commands on the VPS by shelling out to the system `ssh` (so it
// honours ~/.ssh/config, the agent, keys and jump hosts). ControlMaster
// multiplexing is not optional: the read-only panels poll every few seconds and
// issue several commands per poll, and without a shared connection that is an
// SSH-handshake storm.
type SSH struct {
	Host        string // a Host alias from ~/.ssh/config
	ControlPath string // -o ControlPath; defaults to a per-host socket under ~/.ssh
}

func (SSH) Kind() string { return "vps" }

// baseArgs is the ssh invocation without the remote command.
func (s SSH) baseArgs() []string {
	cp := s.ControlPath
	if cp == "" {
		cp = "~/.ssh/cm-edenadmin-%r@%h:%p"
	}
	return []string{
		"ssh",
		"-o", "BatchMode=yes", // never block on a passphrase prompt; fail fast
		"-o", "ConnectTimeout=5",
		"-o", "ControlMaster=auto",
		"-o", "ControlPath=" + cp,
		"-o", "ControlPersist=120",
		s.Host,
		"--",
	}
}

// remote renders the command that runs on the far side. ssh joins its trailing
// arguments with spaces and hands the result to the login shell regardless, so
// we quote every element ourselves and pass exactly one string.
func remote(c Command) string {
	return ShqJoin(c.Argv)
}

func (s SSH) fullArgv(c Command) []string {
	return append(s.baseArgs(), remote(c))
}

func (s SSH) Describe(c Command) string {
	a := s.fullArgv(c)
	// The remote command is already one shell-quoted string; show the ssh argv
	// with that last element wrapped so a reader can copy-paste it.
	head := strings.Join(a[:len(a)-1], " ")
	return head + " " + Shq(a[len(a)-1])
}

func (s SSH) Run(ctx context.Context, c Command) (Result, error) {
	if len(c.Argv) == 0 {
		return Result{}, errors.New("transport: empty argv")
	}
	if s.Host == "" {
		return Result{}, errors.New("transport: ssh profile has no host")
	}
	ctx, cancel := context.WithTimeout(ctx, c.timeout())
	defer cancel()

	argv := s.fullArgv(c)
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	if c.Stdin != nil {
		cmd.Stdin = bytes.NewReader(c.Stdin)
	}
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	err := cmd.Run()
	res := Result{
		Display: s.Describe(c),
		Stdout:  stdout.Bytes(),
		Stderr:  stderr.Bytes(),
	}
	if ctx.Err() == context.DeadlineExceeded {
		return res, &TimeoutError{After: c.timeout()}
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		res.ExitCode = exitErr.ExitCode()
		// ssh maps a remote non-zero exit straight through, but 255 is ssh's
		// own "connection failed" — surface that distinctly.
		if res.ExitCode == 255 {
			return res, classifySSH(res.Stderr)
		}
		return res, nil
	}
	if err != nil {
		return res, classifyExec(err)
	}
	return res, nil
}
