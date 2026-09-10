package parse

import "fmt"

// ParseError is returned when a reply does not match the expected shape. It
// names the parser so an operator-facing message points at the right tool.
type ParseError struct {
	What string
	Msg  string
}

func (e *ParseError) Error() string { return e.What + ": " + e.Msg }

func parseErr(what, format string, a ...any) error {
	return &ParseError{What: what, Msg: fmt.Sprintf(format, a...)}
}
