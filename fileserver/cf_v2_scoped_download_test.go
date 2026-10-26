package main

import "testing"

func TestAuthorizedFileReadScopeOnlySkipsForNativeSingleFileDownload(t *testing.T) {
	tests := []struct {
		name   string
		op     string
		fields map[string]interface{}
		want   bool
	}{
		{"native scoped download", "download", map[string]interface{}{"scope-path": "/docs/a.pdf"}, true},
		{"legacy object-only download", "download", map[string]interface{}{}, false},
		{"empty scope", "download", map[string]interface{}{"scope-path": ""}, false},
		{"wrong typed scope", "download", map[string]interface{}{"scope-path": true}, false},
		{"ZIP", "download-dir", map[string]interface{}{"scope-path": "/docs"}, false},
		{"blocks", "downloadblks", map[string]interface{}{"scope-path": "/docs/a.pdf"}, false},
		{"upload", "upload", map[string]interface{}{"scope-path": "/docs/a.pdf"}, false},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got := hasAuthorizedFileReadScope(test.op, test.fields)
			if got != test.want {
				t.Fatalf("scope decision = %v; expected %v", got, test.want)
			}
		})
	}
}

// ZIP must carry the native Server's path proof; other operations and public
// links keep their existing guards even when a scope property is present.
func TestAuthorizedZipReadScope(t *testing.T) {
	for _, op := range []string{"download-dir", "download-multi"} {
		for _, path := range []string{"/", "/文档/a +b"} {
			if !hasAuthorizedZipReadScope(op, map[string]interface{}{"scope-path": path}) {
				t.Fatalf("scoped %s rejected for %q", op, path)
			}
		}
		for _, value := range []interface{}{nil, true, "", "relative", "/a/../b", "/a//b", "/a\\b", "/a\n"} {
			if hasAuthorizedZipReadScope(op, map[string]interface{}{"scope-path": value}) {
				t.Fatalf("invalid ZIP scope accepted: %#v", value)
			}
		}
	}
	for _, op := range []string{"download", "upload", "downloadblks", "download-dir-link", "download-multi-link"} {
		if hasAuthorizedZipReadScope(op, map[string]interface{}{"scope-path": "/docs"}) {
			t.Fatalf("operation %s admitted as a ZIP", op)
		}
	}
}
