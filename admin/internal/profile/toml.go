package profile

import (
	"fmt"
	"reflect"
	"sort"
	"strings"
)

// This is a deliberately tiny TOML reader/writer for exactly the shape
// profiles.toml has: a top-level `active = "name"` and a set of
// `[profiles.<name>]` tables whose values are strings or single-line string
// arrays. It is strict — a construct it does not recognise is an error, not a
// silent skip — so a malformed file is caught at load rather than at first use.
//
// A full TOML library would be a module dependency (network to fetch, more
// surface to trust) for no benefit here; the format never grows past this.

func decodeSet(data []byte) (Set, error) {
	s := Set{Profiles: map[string]Profile{}}
	var curName string
	var curFields map[string]tomlValue

	flush := func() error {
		if curName == "" {
			return nil
		}
		p := Profile{Name: curName}
		if err := assignFields(&p, curFields); err != nil {
			return fmt.Errorf("[profiles.%s]: %w", curName, err)
		}
		s.Profiles[curName] = p
		curName, curFields = "", nil
		return nil
	}

	for lineno, raw := range strings.Split(string(data), "\n") {
		line := strings.TrimSpace(stripComment(raw))
		if line == "" {
			continue
		}
		if strings.HasPrefix(line, "[") {
			if !strings.HasSuffix(line, "]") {
				return Set{}, fmt.Errorf("line %d: unterminated table header", lineno+1)
			}
			name := strings.TrimSpace(line[1 : len(line)-1])
			if err := flush(); err != nil {
				return Set{}, err
			}
			if name == "profiles" {
				// The parent table. Nothing to do.
				continue
			}
			p, ok := strings.CutPrefix(name, "profiles.")
			if !ok || p == "" {
				return Set{}, fmt.Errorf("line %d: only [profiles.<name>] tables are allowed, got [%s]", lineno+1, name)
			}
			curName = p
			curFields = map[string]tomlValue{}
			continue
		}
		key, val, ok := strings.Cut(line, "=")
		if !ok {
			return Set{}, fmt.Errorf("line %d: expected key = value", lineno+1)
		}
		key = strings.TrimSpace(key)
		tv, err := parseValue(strings.TrimSpace(val))
		if err != nil {
			return Set{}, fmt.Errorf("line %d: %s: %w", lineno+1, key, err)
		}
		if curName == "" {
			if key != "active" {
				return Set{}, fmt.Errorf("line %d: the only top-level key is `active`, got %q", lineno+1, key)
			}
			if tv.list != nil {
				return Set{}, fmt.Errorf("line %d: active must be a string", lineno+1)
			}
			s.Active = tv.str
			continue
		}
		curFields[key] = tv
	}
	if err := flush(); err != nil {
		return Set{}, err
	}
	return s, nil
}

func encodeSet(s Set) []byte {
	var b strings.Builder
	b.WriteString("# edenadmin connection profiles. Mode 0600. No secrets:\n")
	b.WriteString("# ssh keys live in ~/.ssh, the world password lives on the server.\n\n")
	if s.Active != "" {
		fmt.Fprintf(&b, "active = %s\n\n", quoteStr(s.Active))
	}
	names := make([]string, 0, len(s.Profiles))
	for n := range s.Profiles {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		fmt.Fprintf(&b, "[profiles.%s]\n", n)
		writeFields(&b, s.Profiles[n])
		b.WriteString("\n")
	}
	return []byte(b.String())
}

type tomlValue struct {
	str  string
	list []string // non-nil => it was an array
}

func parseValue(s string) (tomlValue, error) {
	if strings.HasPrefix(s, "[") {
		if !strings.HasSuffix(s, "]") {
			return tomlValue{}, fmt.Errorf("unterminated array (must be single-line)")
		}
		inner := strings.TrimSpace(s[1 : len(s)-1])
		out := []string{}
		if inner != "" {
			for _, part := range splitArray(inner) {
				v, err := unquote(strings.TrimSpace(part))
				if err != nil {
					return tomlValue{}, err
				}
				out = append(out, v)
			}
		}
		return tomlValue{list: out}, nil
	}
	v, err := unquote(s)
	if err != nil {
		return tomlValue{}, err
	}
	return tomlValue{str: v}, nil
}

// splitArray splits on commas that are not inside a quoted string.
func splitArray(s string) []string {
	var parts []string
	var cur strings.Builder
	inQuote := false
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"':
			inQuote = !inQuote
			cur.WriteByte(c)
		case c == ',' && !inQuote:
			parts = append(parts, cur.String())
			cur.Reset()
		default:
			cur.WriteByte(c)
		}
	}
	if strings.TrimSpace(cur.String()) != "" || len(parts) > 0 {
		parts = append(parts, cur.String())
	}
	return parts
}

func unquote(s string) (string, error) {
	if len(s) < 2 || s[0] != '"' || s[len(s)-1] != '"' {
		return "", fmt.Errorf("expected a double-quoted string, got %q", s)
	}
	body := s[1 : len(s)-1]
	var b strings.Builder
	for i := 0; i < len(body); i++ {
		if body[i] != '\\' {
			b.WriteByte(body[i])
			continue
		}
		i++
		if i >= len(body) {
			return "", fmt.Errorf("trailing backslash in string")
		}
		switch body[i] {
		case '"':
			b.WriteByte('"')
		case '\\':
			b.WriteByte('\\')
		case 'n':
			b.WriteByte('\n')
		case 't':
			b.WriteByte('\t')
		default:
			return "", fmt.Errorf("unsupported escape \\%c", body[i])
		}
	}
	return b.String(), nil
}

func quoteStr(s string) string {
	r := strings.NewReplacer("\\", `\\`, "\"", `\"`, "\n", `\n`, "\t", `\t`)
	return `"` + r.Replace(s) + `"`
}

func stripComment(line string) string {
	inQuote := false
	for i := 0; i < len(line); i++ {
		switch line[i] {
		case '"':
			inQuote = !inQuote
		case '#':
			if !inQuote {
				return line[:i]
			}
		}
	}
	return line
}

// --- struct <-> fields, driven by the `toml:` tags ---

func assignFields(p *Profile, fields map[string]tomlValue) error {
	v := reflect.ValueOf(p).Elem()
	t := v.Type()
	tagIndex := map[string]int{}
	for i := 0; i < t.NumField(); i++ {
		tag := t.Field(i).Tag.Get("toml")
		if tag == "" || tag == "-" {
			continue
		}
		tagIndex[tag] = i
	}
	for key, tv := range fields {
		idx, ok := tagIndex[key]
		if !ok {
			return fmt.Errorf("unknown key %q", key)
		}
		fv := v.Field(idx)
		switch fv.Kind() {
		case reflect.String:
			if tv.list != nil {
				return fmt.Errorf("%s: expected a string, got an array", key)
			}
			fv.SetString(tv.str)
		case reflect.Slice:
			if tv.list == nil {
				return fmt.Errorf("%s: expected an array, got a string", key)
			}
			fv.Set(reflect.ValueOf(append([]string(nil), tv.list...)))
		default:
			return fmt.Errorf("%s: unsupported field kind %s", key, fv.Kind())
		}
	}
	return nil
}

func writeFields(b *strings.Builder, p Profile) {
	v := reflect.ValueOf(p)
	t := v.Type()
	for i := 0; i < t.NumField(); i++ {
		tag := t.Field(i).Tag.Get("toml")
		if tag == "" || tag == "-" {
			continue
		}
		fv := v.Field(i)
		switch fv.Kind() {
		case reflect.String:
			s := fv.String()
			if s == "" {
				continue
			}
			fmt.Fprintf(b, "%s = %s\n", tag, quoteStr(s))
		case reflect.Slice:
			if fv.Len() == 0 {
				continue
			}
			parts := make([]string, fv.Len())
			for j := 0; j < fv.Len(); j++ {
				parts[j] = quoteStr(fv.Index(j).String())
			}
			fmt.Fprintf(b, "%s = [%s]\n", tag, strings.Join(parts, ", "))
		}
	}
}
