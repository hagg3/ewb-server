package parse

import "strings"

// DuEntry is one line of `du -k --max-depth=1 <dir>`: a size in KiB and a path.
type DuEntry struct {
	KBytes int64  `json:"kbytes"`
	Path   string `json:"path"`
}

// DuDirs parses `du -k --max-depth=1 <dir>` output. Each line is
// "<kbytes>\t<path>" (some du builds pad with spaces instead of a tab); the
// caller separates the trailing total line (Path == the queried directory) from
// the per-subdirectory rows. An unparseable line is skipped rather than failing
// the whole read — a backup listing must not disappear because of one odd row.
func DuDirs(b []byte) ([]DuEntry, error) {
	var out []DuEntry
	for _, ln := range lines(b) {
		fields := strings.SplitN(strings.TrimSpace(ln), "\t", 2)
		if len(fields) != 2 {
			f := strings.Fields(ln)
			if len(f) < 2 {
				continue
			}
			fields = []string{f[0], strings.Join(f[1:], " ")}
		}
		n, err := atoiComma(fields[0])
		if err != nil {
			continue
		}
		out = append(out, DuEntry{KBytes: n, Path: strings.TrimSpace(fields[1])})
	}
	if len(out) == 0 {
		return nil, parseErr("du", "no parseable lines")
	}
	return out, nil
}
