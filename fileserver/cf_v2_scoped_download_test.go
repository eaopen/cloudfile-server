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
