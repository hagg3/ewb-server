package panels

import (
	"path/filepath"
	"strconv"
)

// WorldArgs is the port / world file / server name pulled out of a local
// profile's edenserver argv. Plan §3.2: for a local profile these come from the
// args, not from a running process or a conf file.
type WorldArgs struct {
	Port  int    `json:"port"`
	World string `json:"world"`
	Name  string `json:"name"`
}

// parseWorldArgs reads the subset of edenserver flags the Status panel shows. A
// bare leading number is still accepted as the port (server_posix.cpp
// back-compat). Unknown flags are skipped; a flag that takes a value consumes
// the next token so its argument is not misread as a bare port.
func parseWorldArgs(argv []string) WorldArgs {
	wa := WorldArgs{Port: 27015}
	takesValue := map[string]bool{
		"--port": true, "--name": true, "--password": true, "--world": true,
		"--signs": true, "--spawn-file": true, "--spawn": true, "--max-world-cells": true,
		"--matchmaker": true, "--advertise": true, "--control-socket": true,
		"--ban-file": true, "--ops-file": true, "--default-level": true,
		"--audit-file": true, "--region-radius": true, "--connect-limit": true,
	}
	for i := 0; i < len(argv); i++ {
		a := argv[i]
		switch a {
		case "--port":
			if i+1 < len(argv) {
				if n, err := strconv.Atoi(argv[i+1]); err == nil {
					wa.Port = n
				}
			}
		case "--name":
			if i+1 < len(argv) {
				wa.Name = argv[i+1]
			}
		case "--world":
			if i+1 < len(argv) {
				wa.World = argv[i+1]
			}
		default:
			if a != "" && a[0] != '-' && wa.portFromBare(a) {
				continue
			}
		}
		if takesValue[a] {
			i++
		}
	}
	return wa
}

func (wa *WorldArgs) portFromBare(tok string) bool {
	n, err := strconv.Atoi(tok)
	if err != nil || n <= 0 || n > 65535 {
		return false
	}
	wa.Port = n
	return true
}

// WithActiveWorld rewrites a local profile's edenserver args to point at
// worldDir: `--world <worldDir>/eden_world.model`, and `--signs
// <worldDir>/eden_signs.txt` when that sidecar exists (dropped otherwise, so a
// stale --signs from the previous world does not leak in). The server derives
// the control socket, eden_spawn.txt and eden_players.txt from the --world
// directory, so this one flag moves the whole world (roadmap 6.4, plan §3.7).
func WithActiveWorld(args []string, worldDir string, hasSigns bool) []string {
	out := setFlag(args, "--world", filepath.Join(worldDir, "eden_world.model"))
	if hasSigns {
		out = setFlag(out, "--signs", filepath.Join(worldDir, "eden_signs.txt"))
	} else {
		out = removeFlag(out, "--signs")
	}
	return out
}

// setFlag replaces the value token after flag, or appends `flag value` if the
// flag is not present.
func setFlag(args []string, flag, value string) []string {
	out := make([]string, 0, len(args)+2)
	replaced := false
	for i := 0; i < len(args); i++ {
		if args[i] == flag {
			out = append(out, flag, value)
			i++ // skip the old value
			replaced = true
			continue
		}
		out = append(out, args[i])
	}
	if !replaced {
		out = append(out, flag, value)
	}
	return out
}

// removeFlag drops flag and the value token that follows it.
func removeFlag(args []string, flag string) []string {
	out := make([]string, 0, len(args))
	for i := 0; i < len(args); i++ {
		if args[i] == flag {
			i++ // skip the value too
			continue
		}
		out = append(out, args[i])
	}
	return out
}
