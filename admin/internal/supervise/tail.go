package supervise

import (
	"io"
	"os"
)

// LogTail is a resumable reader over a growing log file. The Logs panel keeps
// one and polls it: each Read returns the bytes written since the last call.
// If the file shrank (truncation or a rotation swapped in a smaller file) the
// offset resets to 0 so nothing is skipped and nothing is misaligned.
type LogTail struct {
	Path   string
	offset int64
}

// Result is one poll of the tail.
type Result struct {
	Data    []byte `json:"-"`
	Text    string `json:"text"`
	Offset  int64  `json:"offset"`
	Rotated bool   `json:"rotated"`
	Missing bool   `json:"missing"`
}

// Read returns everything appended since the previous Read. maxBytes caps a
// single response so a huge backlog on first load cannot blow the HTTP reply;
// when the backlog is larger, the oldest bytes are dropped and the offset jumps
// to the tail of what was returned.
func (t *LogTail) Read(maxBytes int64) (Result, error) {
	f, err := os.Open(t.Path)
	if err != nil {
		if os.IsNotExist(err) {
			return Result{Missing: true, Offset: t.offset}, nil
		}
		return Result{}, err
	}
	defer f.Close()

	fi, err := f.Stat()
	if err != nil {
		return Result{}, err
	}
	size := fi.Size()

	res := Result{}
	start := t.offset
	if size < start {
		// Truncated or rotated: start over from the beginning of the new file.
		start = 0
		res.Rotated = true
	}
	if maxBytes > 0 && size-start > maxBytes {
		start = size - maxBytes
	}
	if start > 0 {
		if _, err := f.Seek(start, io.SeekStart); err != nil {
			return Result{}, err
		}
	}
	data, err := io.ReadAll(f)
	if err != nil {
		return Result{}, err
	}
	t.offset = size
	res.Data = data
	res.Text = string(data)
	res.Offset = size
	return res, nil
}

// Reset rewinds the tail so the next Read returns the whole file.
func (t *LogTail) Reset() { t.offset = 0 }
