// Package transport turns a panel's []string argv into an executed command.
//
// The one architectural rule (plan §1): panels build argv. Only this package
// ever turns argv into a *string*, and only for the ssh path. The local path
// hands argv straight to execve with no shell in the loop. Every quoting bug in
// edenadmin is therefore a bug in Shq below, which has a table-driven test,
// rather than a bug scattered across the panels.
package transport

import "strings"

// Shq renders one argument as a POSIX shell word that expands back to exactly
// the input bytes. It is the single escaper in the program.
//
// Strategy: wrap in single quotes (inside which every byte except ' is
// literal) and rewrite each embedded ' as '\'' — close the quote, an escaped
// literal quote, reopen. The empty string becomes '' rather than nothing, so a
// deliberately-empty argument survives the round trip.
func Shq(s string) string {
	if s == "" {
		return "''"
	}
	// Fast path: an argument made only of unambiguously-safe bytes needs no
	// quoting at all. This keeps the common `ssh host -- edenctl -S /path who`
	// readable in the Connection panel's "exact command" display.
	if isShellSafe(s) {
		return s
	}
	var b strings.Builder
	b.Grow(len(s) + 2)
	b.WriteByte('\'')
	for i := 0; i < len(s); i++ {
		if s[i] == '\'' {
			b.WriteString(`'\''`)
			continue
		}
		b.WriteByte(s[i])
	}
	b.WriteByte('\'')
	return b.String()
}

// ShqJoin renders a whole argv as a single shell command string.
func ShqJoin(argv []string) string {
	parts := make([]string, len(argv))
	for i, a := range argv {
		parts[i] = Shq(a)
	}
	return strings.Join(parts, " ")
}

func isShellSafe(s string) bool {
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'A' && c <= 'Z', c >= 'a' && c <= 'z', c >= '0' && c <= '9':
		case c == '-' || c == '_' || c == '/' || c == '.' || c == ':' || c == '@' || c == '=' || c == ',' || c == '+':
		default:
			return false
		}
	}
	return true
}
