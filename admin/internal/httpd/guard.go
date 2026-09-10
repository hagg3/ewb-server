package httpd

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"net"
	"net/http"
	"strings"
)

// NewToken returns a fresh 32-byte per-run bearer token, base64url. It is
// handed to the browser as the URL fragment on the launch URL (#t=...), stashed
// in sessionStorage by app.js, and sent as the X-Edenadmin-Token header on
// every request except GET / and the static assets. It is never put in a query
// string (those land in logs).
func NewToken() string {
	var b [32]byte
	if _, err := rand.Read(b[:]); err != nil {
		panic("edenadmin: no entropy for the session token: " + err.Error())
	}
	return base64.RawURLEncoding.EncodeToString(b[:])
}

// guard wraps a handler with the four checks from the plan's §1 table. Loopback
// bind alone is not enough: any local process, and any web page the operator
// visits, can reach 127.0.0.1.
type guard struct {
	token    string
	allowed  map[string]bool // Host / Origin-host values that are us
	next     http.Handler
	openPath func(string) bool // paths served without a token (GET / and assets)
}

func (g *guard) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	// 1. The connection must come from loopback. Rejects an off-box client and
	//    a mis-set bind address.
	if !isLoopback(r.RemoteAddr) {
		http.Error(w, "edenadmin: non-loopback client refused", http.StatusForbidden)
		return
	}

	// 2. DNS-rebinding guard: the Host header (and Origin, if present) must be
	//    one of our own loopback authorities.
	if !g.allowed[r.Host] {
		http.Error(w, "edenadmin: unexpected Host header", http.StatusForbidden)
		return
	}
	if o := r.Header.Get("Origin"); o != "" {
		if !g.allowed[originHost(o)] {
			http.Error(w, "edenadmin: cross-origin request refused", http.StatusForbidden)
			return
		}
	}

	open := g.openPath != nil && g.openPath(r.URL.Path)

	// 3. Every state change is POST. A GET on a mutating route is a bug or an
	//    attack (an <img> tag, say); reject before the token check so the
	//    message is accurate.
	if !open && r.Method != http.MethodGet && r.Method != http.MethodPost {
		http.Error(w, "edenadmin: method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if strings.HasPrefix(r.URL.Path, "/api/") && isMutatingRoute(r.URL.Path) && r.Method != http.MethodPost {
		http.Error(w, "edenadmin: this route requires POST", http.StatusMethodNotAllowed)
		return
	}

	// 4. Token on everything except the open paths. An HTML form cannot set a
	//    custom header cross-origin, so this also closes form-CSRF.
	if !open {
		got := r.Header.Get("X-Edenadmin-Token")
		if got == "" || subtle.ConstantTimeCompare([]byte(got), []byte(g.token)) != 1 {
			http.Error(w, "edenadmin: missing or bad session token", http.StatusUnauthorized)
			return
		}
	}

	g.next.ServeHTTP(w, r)
}

func isLoopback(remoteAddr string) bool {
	host, _, err := net.SplitHostPort(remoteAddr)
	if err != nil {
		host = remoteAddr
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

func originHost(o string) string {
	// Origin is scheme://host[:port]. Strip the scheme.
	if i := strings.Index(o, "://"); i >= 0 {
		return o[i+3:]
	}
	return o
}

// readOnlyAPI is the allowlist of GET-able /api routes. Everything else under
// /api/ changes state and is POST-only — listing the safe routes rather than the
// mutating ones means a newly added action route is POST-guarded by default.
var readOnlyAPI = map[string]bool{
	"/api/state":   true,
	"/api/status":  true,
	"/api/players": true,
	"/api/logs":    true,
	"/api/audit":   true,
	"/api/bans":    true, // GET reads the ban list; POST /api/bans mutates and still needs the token
	"/api/worlds":  true,
	// GET renders the Config form; POST /api/config writes the EnvironmentFile
	// (or the profile args) and still needs the token.
	"/api/config": true,
	// GET downloads a world bundle; POST /api/worlds/bundle uploads one and still
	// needs the token (and the server-stopped check, in the handler).
	"/api/worlds/bundle": true,
	// GET lists backups; POST /api/backups/create and /api/backups/restore
	// mutate and are POST-only by default (not in this allowlist).
	"/api/backups": true,
}

func isMutatingRoute(path string) bool {
	return strings.HasPrefix(path, "/api/") && !readOnlyAPI[path]
}
