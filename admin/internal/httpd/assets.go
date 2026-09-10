package httpd

import (
	"bytes"
	"embed"
	"io/fs"
	"net/http"
	"strings"
	"time"
)

//go:embed web
var webFS embed.FS

var buildTime = time.Now()

// assets serves the embedded browser shell. Only three files exist (index.html,
// app.js, style.css); anything else is a 404. We serve the bytes directly
// rather than via http.FileServer, whose /index.html <-> / redirect dance
// fights the loopback guard.
func assetsHandler() (http.Handler, error) {
	sub, err := fs.Sub(webFS, "web")
	if err != nil {
		return nil, err
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		name := strings.TrimPrefix(r.URL.Path, "/")
		if name == "" {
			name = "index.html"
		}
		data, err := fs.ReadFile(sub, name)
		if err != nil {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", contentType(name))
		w.Header().Set("Cache-Control", "no-cache")
		http.ServeContent(w, r, name, buildTime, bytes.NewReader(data))
	}), nil
}

func contentType(name string) string {
	switch {
	case strings.HasSuffix(name, ".html"):
		return "text/html; charset=utf-8"
	case strings.HasSuffix(name, ".js"):
		return "text/javascript; charset=utf-8"
	case strings.HasSuffix(name, ".css"):
		return "text/css; charset=utf-8"
	default:
		return "application/octet-stream"
	}
}

// isAssetPath is true for the paths served without a token.
func isAssetPath(p string) bool {
	switch p {
	case "/", "/index.html", "/app.js", "/style.css":
		return true
	}
	return false
}
