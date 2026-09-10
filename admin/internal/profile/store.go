package profile

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// DefaultPath is ~/.config/edenadmin/profiles.toml.
func DefaultPath() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".config", "edenadmin", "profiles.toml"), nil
}

// Load reads the profile set. A missing file is not an error — it returns an
// empty set so a first run lands on the Connection panel with nothing
// configured yet.
func Load(path string) (Set, error) {
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return Set{Profiles: map[string]Profile{}}, nil
	}
	if err != nil {
		return Set{}, err
	}
	s, err := decodeSet(data)
	if err != nil {
		return Set{}, fmt.Errorf("%s: %w", path, err)
	}
	if err := s.Validate(); err != nil {
		return Set{}, fmt.Errorf("%s: %w", path, err)
	}
	return s, nil
}

// Save writes the set atomically at mode 0600. It validates first so a bad
// profile never reaches disk.
func Save(path string, s Set) error {
	if err := s.Validate(); err != nil {
		return err
	}
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(dir, ".profiles-*.toml")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName) // no-op after a successful rename

	if err := tmp.Chmod(0o600); err != nil {
		tmp.Close()
		return err
	}
	if _, err := tmp.Write(encodeSet(s)); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpName, path)
}
