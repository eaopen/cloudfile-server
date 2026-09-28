// Cross-language contract checks for the write lifecycle seam.
//
// The Go side does not classify operations -- it asks seaf-server. So what
// there is to test here is not logic but agreement: that Go's vocabulary is
// exactly C's, and that the JSON field names Go emits are exactly the ones
// cf-fileop-json.c reads. Those are the two ways this seam can break silently.
//
// A typo in a Go operation constant would sail past the compiler, past go vet
// and past a smoke test: seaf-server would refuse the unknown operation, the
// upload would fail with a generic error, and nothing would point at the
// constant. A renamed JSON key is worse -- the field simply arrives empty, so
// a lock keyed on a path would adjudicate the wrong object, or none.
//
// Run with: go test ./... -run CfFileOp

package main

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// goOperations is every operation constant this package defines. Kept as a
// literal rather than derived: deriving it from the same source the test
// checks would make the test agree with itself.
var goOperations = []string{
	cfOpCreateFile,
	cfOpUpdateFile,
	cfOpDelete,
	cfOpMkdir,
	cfOpRename,
	cfOpMove,
	cfOpCopy,
	cfOpRevertFile,
	cfOpRevertDir,
	cfOpRevertRepo,
	cfOpUpdateDir,
	cfOpUploadBlocks,
	cfOpSyncUpdate,
}

func repoRoot(t *testing.T) string {
	t.Helper()
	root, err := filepath.Abs("..")
	if err != nil {
		t.Fatalf("failed to resolve repo root: %v", err)
	}
	return root
}

func readFile(t *testing.T, path string) string {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("failed to read %s: %v", path, err)
	}
	return string(data)
}

// TestCfFileOpVocabularyMatchesC checks Go's constants against the #defines in
// common/cf-fileop.h, in both directions.
func TestCfFileOpVocabularyMatchesC(t *testing.T) {
	header := readFile(t, filepath.Join(repoRoot(t), "common", "cf-fileop.h"))

	re := regexp.MustCompile(`#define (CF_OP_\w+)\s+"([^"]+)"`)
	cOps := make(map[string]string)
	for _, m := range re.FindAllStringSubmatch(header, -1) {
		cOps[m[2]] = m[1]
	}

	if len(cOps) == 0 {
		t.Fatal("found no CF_OP_* defines in cf-fileop.h")
	}

	for _, op := range goOperations {
		if _, ok := cOps[op]; !ok {
			t.Errorf("Go defines operation %q, C does not", op)
		}
	}

	goSet := make(map[string]bool, len(goOperations))
	for _, op := range goOperations {
		goSet[op] = true
	}
	for value, name := range cOps {
		if !goSet[value] {
			t.Errorf("C defines %s = %q, Go does not", name, value)
		}
	}
}

// TestCfFileOpJSONKeysMatchC checks that every key the Go struct emits is a
// key cf-fileop-json.c actually reads, and vice versa. A key that only one
// side knows about is silently dropped -- no error, no log, just an empty
// field where a path should have been.
func TestCfFileOpJSONKeysMatchC(t *testing.T) {
	source := readFile(t, filepath.Join(repoRoot(t), "common", "cf-fileop-json.c"))

	re := regexp.MustCompile(`dup_string_(?:member|array) \(obj, "(\w+)"\)`)
	cKeys := make(map[string]bool)
	for _, m := range re.FindAllStringSubmatch(source, -1) {
		cKeys[m[1]] = true
	}
	if len(cKeys) == 0 {
		t.Fatal("found no JSON keys in cf-fileop-json.c")
	}

	goKeys := make(map[string]bool)
	typ := reflect.TypeOf(cfFileOp{})
	for i := 0; i < typ.NumField(); i++ {
		tag := typ.Field(i).Tag.Get("json")
		if tag == "" || tag == "-" {
			t.Errorf("field %s has no json tag", typ.Field(i).Name)
			continue
		}
		goKeys[strings.Split(tag, ",")[0]] = true
	}

	for key := range goKeys {
		if !cKeys[key] {
			t.Errorf("Go emits JSON key %q, cf-fileop-json.c does not read it", key)
		}
	}
	for key := range cKeys {
		if !goKeys[key] {
			t.Errorf("cf-fileop-json.c reads JSON key %q, Go never emits it", key)
		}
	}
}

// TestCfFileOpErrorCodesMatchC guards the two CloudFile error codes, which are
// the only thing turning a refusal into a 423 rather than a 403.
func TestCfFileOpErrorCodesMatchC(t *testing.T) {
	header := readFile(t, filepath.Join(repoRoot(t), "common", "cf-fileop.h"))

	for name, want := range map[string]int{
		"CF_ERR_FILE_LOCKED":      cfErrFileLocked,
		"CF_ERR_VERSION_MISMATCH": cfErrVersionMismatch,
	} {
		re := regexp.MustCompile(`#define ` + name + `\s+(\d+)`)
		m := re.FindStringSubmatch(header)
		if m == nil {
			t.Errorf("%s is not defined in cf-fileop.h", name)
			continue
		}
		if m[1] != itoa(want) {
			t.Errorf("%s is %s in C but %d in Go", name, m[1], want)
		}
	}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var digits []byte
	for n > 0 {
		digits = append([]byte{byte('0' + n%10)}, digits...)
		n /= 10
	}
	return string(digits)
}

func TestCfFileOpHTTPStatus(t *testing.T) {
	cases := []struct {
		code int
		want int
	}{
		{cfErrFileLocked, http.StatusLocked},
		{cfErrVersionMismatch, http.StatusConflict},
		{500, http.StatusForbidden},
		{0, http.StatusForbidden},
	}
	for _, c := range cases {
		if got := cfHTTPStatus(c.code); got != c.want {
			t.Errorf("cfHTTPStatus(%d) = %d, want %d", c.code, got, c.want)
		}
	}
}

// TestCfFileOpVerdictDecoding covers what comes back over the wire, including
// the shapes that must NOT be read as permission to write.
func TestCfFileOpVerdictDecoding(t *testing.T) {
	cases := []struct {
		name        string
		raw         string
		wantAllowed bool
		wantCode    int
	}{
		{"allow", `{"allowed":true}`, true, 0},
		{"refuse locked", `{"allowed":false,"code":600,"message":"Locked by alice"}`, false, 600},
		{"refuse generic", `{"allowed":false,"code":500,"message":"no"}`, false, 500},
		// An empty object decodes with Allowed false. That is the safe
		// default and the test exists to keep it that way: a verdict Go
		// cannot understand must never mean yes.
		{"empty object", `{}`, false, 0},
	}

	for _, c := range cases {
		var v cfVerdict
		if err := json.Unmarshal([]byte(c.raw), &v); err != nil {
			t.Errorf("%s: unexpected decode error: %v", c.name, err)
			continue
		}
		if v.Allowed != c.wantAllowed {
			t.Errorf("%s: allowed = %v, want %v", c.name, v.Allowed, c.wantAllowed)
		}
		if v.Code != c.wantCode {
			t.Errorf("%s: code = %d, want %d", c.name, v.Code, c.wantCode)
		}
	}
}

// TestCfFileOpSharedCaseSet drives the Go vocabulary from the same file the C
// suite uses, so the two cannot be updated apart. Skipped when the
// cloudfile-docker repo is not checked out beside this one.
func TestCfFileOpSharedCaseSet(t *testing.T) {
	path := os.Getenv("CF_FILEOP_CASES")
	if path == "" {
		root := repoRoot(t)
		path = filepath.Join(filepath.Dir(root), "cloudfile-docker",
			"docs", "fileop-cases.json")
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Skipf("shared case set not found at %s; set CF_FILEOP_CASES", path)
	}

	var cases struct {
		Operations struct {
			Cases []struct {
				Op    string `json:"op"`
				Valid bool   `json:"valid"`
			} `json:"cases"`
		} `json:"operations"`
	}
	if err := json.Unmarshal(data, &cases); err != nil {
		t.Fatalf("failed to parse %s: %v", path, err)
	}

	var wantValid []string
	for _, c := range cases.Operations.Cases {
		if c.Valid {
			wantValid = append(wantValid, c.Op)
		}
	}
	if len(wantValid) == 0 {
		t.Fatal("shared case set lists no valid operations")
	}

	got := append([]string(nil), goOperations...)
	sort.Strings(got)
	sort.Strings(wantValid)

	if !reflect.DeepEqual(got, wantValid) {
		t.Errorf("Go vocabulary does not match the shared case set:\n got: %v\nwant: %v",
			got, wantValid)
	}
}
