package parse

import (
	"strconv"
	"strings"
)

// ImportSummary is the parsed projection of one eden_import run. The summary
// block goes to stdout; the verdict lines ("eden_import: error: ...",
// "eden_import: warning: ...") go to stderr; a non-zero exit means the tool
// refused to write. The Worlds panel renders this as the confirm gate the
// operator approves against, so the numbers must be exact.
type ImportSummary struct {
	World       string `json:"world"` // "(unnamed)" if the .eden carried no name
	Version     int    `json:"version"`
	ZCeiling    int    `json:"z_ceiling"` // "64z" -> 64
	Chunks      int    `json:"chunks"`
	SourceSigns int    `json:"source_signs"`
	SignSource  string `json:"sign_source"` // "none" / "sidecar" / "inline"

	Empty bool   `json:"empty"` // "(nothing to emit)"
	BBox  [6]int `json:"bbox"`  // x0,x1,y0,y1,z0,z1 (zero when Empty)

	Strategy    string `json:"strategy"` // diff / solid / full
	BaseProfile string `json:"base_profile"`

	Cells int64 `json:"cells"`
	Cap   int64 `json:"cap"`

	WorstRegionRecords int64  `json:"worst_region_records"`
	WorstRegionBox     [4]int `json:"worst_region_box"` // x0,x1,z0,z1 (zero if no records)

	SignsConvertible int `json:"signs_convertible"`
	SignsDropped     int `json:"signs_dropped"`

	HasSpawn    bool    `json:"has_spawn"`
	SpawnX      float64 `json:"spawn_x"`
	SpawnY      float64 `json:"spawn_y"`
	SpawnZ      float64 `json:"spawn_z"`
	SpawnSource string  `json:"spawn_source"`

	DryRun bool     `json:"dry_run"`
	Wrote  []string `json:"wrote,omitempty"` // paths from the "wrote ..." lines on a real run

	Verdicts []Verdict `json:"verdicts,omitempty"`
	Refused  bool      `json:"refused"` // exit code != 0

	Stdout string `json:"stdout"` // kept verbatim for display
	Stderr string `json:"stderr"`
}

// Verdict is one "eden_import: error|warning: ..." line, classified so the panel
// can offer the right override (a raised --max-* flag) or none (type 255).
type Verdict struct {
	Level string `json:"level"` // "error" | "warning"
	Kind  string `json:"kind"`  // see the classify() cases
	Text  string `json:"text"`  // the message verbatim, minus the "eden_import: <level>: " prefix
}

// Verdict kinds.
const (
	VerdictCellsOverCap       = "cells-over-cap"
	VerdictRegionOverCeiling  = "region-over-ceiling"
	VerdictRegionOverObserved = "region-over-observed"
	VerdictType255            = "type-255"
	VerdictBadBlockType       = "bad-block-type"
	VerdictBadPaint           = "bad-paint"
	VerdictOther              = "other"
)

// ImportSummaryParse parses one eden_import invocation.
func ImportSummaryParse(stdout, stderr []byte, exitCode int) (ImportSummary, error) {
	s := ImportSummary{
		Stdout:  string(stdout),
		Stderr:  string(stderr),
		Refused: exitCode != 0,
	}
	ls := lines(stdout)
	if len(ls) == 0 || !strings.HasPrefix(ls[0], "world ") {
		// A refusal that failed before the summary (unreadable file) has only
		// stderr. Still return the verdicts so the panel can show them.
		s.Verdicts = parseVerdicts(stderr)
		if len(s.Verdicts) > 0 || strings.TrimSpace(s.Stderr) != "" {
			return s, nil
		}
		return s, parseErr("import", "no summary and no verdict on stderr")
	}

	parseWorldLine(&s, ls[0])
	for _, row := range ls[1:] {
		t := strings.TrimSpace(row)
		switch {
		case strings.HasPrefix(t, "bounding box"):
			parseBBox(&s, t)
		case strings.HasPrefix(t, "strategy"):
			rest := strings.TrimSpace(strings.TrimPrefix(t, "strategy"))
			s.Strategy, _, _ = strings.Cut(rest, " ")
			if i := strings.Index(rest, "base profile: "); i >= 0 {
				s.BaseProfile = strings.TrimRight(rest[i+len("base profile: "):], ")")
			}
		case strings.HasPrefix(t, "cells"):
			parseCells(&s, t)
		case strings.HasPrefix(t, "worst REGION"):
			parseWorstRegion(&s, t)
		case strings.HasPrefix(t, "signs "):
			parseSigns(&s, t)
		case strings.HasPrefix(t, "spawn "):
			parseSpawn(&s, strings.TrimSpace(strings.TrimPrefix(t, "spawn")))
		case strings.HasPrefix(t, "wrote "):
			f := strings.Fields(t)
			if len(f) >= 2 {
				s.Wrote = append(s.Wrote, f[1])
			}
		case strings.HasPrefix(t, "--dry-run:"):
			s.DryRun = true
		}
	}
	s.Verdicts = parseVerdicts(stderr)
	return s, nil
}

func parseWorldLine(s *ImportSummary, line string) {
	// world "Name"  (version 4, 64z, 38 saved chunks, 0 signs from none)
	rest := strings.TrimPrefix(line, "world ")
	if i := strings.Index(rest, "  ("); i >= 0 {
		name := strings.TrimSpace(rest[:i])
		s.World = strings.Trim(name, `"`)
		inner := strings.TrimSuffix(strings.TrimSpace(rest[i+3:]), ")")
		for _, part := range strings.Split(inner, ",") {
			p := strings.TrimSpace(part)
			switch {
			case strings.HasPrefix(p, "version "):
				s.Version, _ = strconv.Atoi(strings.TrimPrefix(p, "version "))
			case strings.HasSuffix(p, "z"):
				z, err := strconv.Atoi(strings.TrimSuffix(p, "z"))
				if err == nil {
					s.ZCeiling = z
				}
			case strings.Contains(p, "saved chunk"):
				s.Chunks = leadingInt(p)
			case strings.Contains(p, "sign"):
				s.SourceSigns = leadingInt(p)
				if i := strings.Index(p, " from "); i >= 0 {
					s.SignSource = p[i+len(" from "):]
				}
			}
		}
	}
}

func parseBBox(s *ImportSummary, t string) {
	if strings.Contains(t, "nothing to emit") {
		s.Empty = true
		return
	}
	// bounding box  x 64714..64815   y 32..53   z 65655..65757   (server coords)
	f := strings.Fields(t)
	axis := map[string][2]int{}
	for i := 0; i+1 < len(f); i++ {
		if (f[i] == "x" || f[i] == "y" || f[i] == "z") && strings.Contains(f[i+1], "..") {
			lo, hi, _ := strings.Cut(f[i+1], "..")
			a, _ := strconv.Atoi(lo)
			b, _ := strconv.Atoi(hi)
			axis[f[i]] = [2]int{a, b}
		}
	}
	s.BBox = [6]int{axis["x"][0], axis["x"][1], axis["y"][0], axis["y"][1], axis["z"][0], axis["z"][1]}
}

func parseCells(s *ImportSummary, t string) {
	// cells         3,433  of 4,000,000 cap  (0.1%)
	rest := strings.TrimSpace(strings.TrimPrefix(t, "cells"))
	num, after, ok := strings.Cut(rest, "  of ")
	if !ok {
		return
	}
	s.Cells, _ = atoiComma(num)
	capStr := strings.TrimSpace(after)
	if i := strings.Index(capStr, " cap"); i >= 0 {
		capStr = capStr[:i]
	}
	s.Cap, _ = atoiComma(capStr)
}

func parseWorstRegion(s *ImportSummary, t string) {
	// worst REGION  5,439 records  at x 64704..65167, z 65648..66111
	rest := strings.TrimSpace(strings.TrimPrefix(t, "worst REGION"))
	num, after, _ := strings.Cut(rest, " records")
	s.WorstRegionRecords, _ = atoiComma(num)
	if i := strings.Index(after, "at x "); i >= 0 {
		box := after[i+len("at x "):]
		xPart, zPart, ok := strings.Cut(box, ", z ")
		if ok {
			x0, x1 := splitRange(xPart)
			z0, z1 := splitRange(zPart)
			s.WorstRegionBox = [4]int{x0, x1, z0, z1}
		}
	}
}

func parseSigns(s *ImportSummary, t string) {
	// signs         0 convertible[, N out of range and dropped]
	rest := strings.TrimSpace(strings.TrimPrefix(t, "signs"))
	s.SignsConvertible = leadingInt(rest)
	if i := strings.Index(rest, ", "); i >= 0 {
		s.SignsDropped = leadingInt(rest[i+2:])
	}
}

func parseSpawn(s *ImportSummary, rest string) {
	if rest == "none" || strings.HasPrefix(rest, "none") {
		return
	}
	// 64746.69, 34.92, 65710.99  (header)
	coords, src, _ := strings.Cut(rest, "  (")
	parts := strings.Split(coords, ",")
	if len(parts) != 3 {
		return
	}
	x, e1 := strconv.ParseFloat(strings.TrimSpace(parts[0]), 64)
	y, e2 := strconv.ParseFloat(strings.TrimSpace(parts[1]), 64)
	z, e3 := strconv.ParseFloat(strings.TrimSpace(parts[2]), 64)
	if e1 != nil || e2 != nil || e3 != nil {
		return
	}
	s.HasSpawn = true
	s.SpawnX, s.SpawnY, s.SpawnZ = x, y, z
	s.SpawnSource = strings.TrimRight(src, ")")
}

func parseVerdicts(stderr []byte) []Verdict {
	var out []Verdict
	for _, row := range lines(stderr) {
		const p = "eden_import: "
		if !strings.HasPrefix(row, p) {
			continue
		}
		rest := row[len(p):]
		level, msg, ok := strings.Cut(rest, ": ")
		if !ok || (level != "error" && level != "warning") {
			continue
		}
		out = append(out, Verdict{Level: level, Kind: classifyVerdict(msg), Text: msg})
	}
	return out
}

func classifyVerdict(msg string) string {
	switch {
	case strings.Contains(msg, "block type 255"):
		return VerdictType255
	case strings.Contains(msg, "cells exceeds the"):
		return VerdictCellsOverCap
	case strings.Contains(msg, "over the") && strings.Contains(msg, "ceiling"):
		return VerdictRegionOverCeiling
	case strings.Contains(msg, "larger than any reply ever captured"):
		return VerdictRegionOverObserved
	case strings.Contains(msg, "block id above"):
		return VerdictBadBlockType
	case strings.Contains(msg, "paint above"):
		return VerdictBadPaint
	default:
		return VerdictOther
	}
}

// --- small helpers --------------------------------------------------------

func leadingInt(s string) int {
	s = strings.TrimSpace(s)
	i := 0
	for i < len(s) && (s[i] == ',' || (s[i] >= '0' && s[i] <= '9')) {
		i++
	}
	n, _ := atoiComma(s[:i])
	return int(n)
}

func splitRange(s string) (int, int) {
	lo, hi, _ := strings.Cut(strings.TrimSpace(s), "..")
	a, _ := strconv.Atoi(strings.TrimSpace(lo))
	b, _ := strconv.Atoi(strings.TrimSpace(hi))
	return a, b
}
