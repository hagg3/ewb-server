// Command edenadmin serves a local operator GUI for the standalone Eden server
// at http://127.0.0.1:<port>. It drives the existing tools — edenctl over the
// control socket, edenserverctl / systemctl / journalctl over ssh, and
// eden_import for .eden world conversion. No new listening port on the VPS and
// no new credential: admin access is possession of the SSH key.
//
// Through stage 6.5: the HTTP shell + loopback guard, profile storage, and the
// Connection / Status / Players / Logs / Power / Bans / Worlds / Backups panels.
// Config and Audit arrive with 6.6–6.7.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"syscall"

	"github.com/hagg3/ewb-server/admin/internal/httpd"
	"github.com/hagg3/ewb-server/admin/internal/profile"
)

func main() {
	port := flag.Int("port", 0, "loopback port to bind (0 = an ephemeral port)")
	profilePath := flag.String("profiles", "", "path to profiles.toml (default: ~/.config/edenadmin/profiles.toml)")
	open := flag.Bool("open", true, "open the URL in the default browser")
	flag.Parse()

	path := *profilePath
	if path == "" {
		p, err := profile.DefaultPath()
		if err != nil {
			die("cannot locate the config directory: %v", err)
		}
		path = p
	}

	srv, err := httpd.New(*port, path)
	if err != nil {
		die("%v", err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	url := srv.URL()
	fmt.Printf("edenadmin — %s\n", url)
	fmt.Printf("profiles:  %s\n", path)
	fmt.Println("Ctrl-C to stop.")

	if *open {
		if err := openBrowser(url); err != nil {
			fmt.Fprintf(os.Stderr, "edenadmin: could not open a browser (%v) — open the URL above yourself\n", err)
		}
	}

	if err := srv.Serve(ctx); err != nil {
		die("%v", err)
	}
	fmt.Println("edenadmin: stopped.")
}

func openBrowser(url string) error {
	var cmd string
	var args []string
	switch runtime.GOOS {
	case "darwin":
		cmd, args = "open", []string{url}
	case "windows":
		cmd, args = "rundll32", []string{"url.dll,FileProtocolHandler", url}
	default:
		cmd, args = "xdg-open", []string{url}
	}
	return exec.Command(cmd, args...).Start()
}

func die(format string, a ...any) {
	fmt.Fprintf(os.Stderr, "edenadmin: "+format+"\n", a...)
	os.Exit(1)
}
