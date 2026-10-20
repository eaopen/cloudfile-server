package option

import (
	"os"
	"path/filepath"
	"testing"
)

// The existing Docker setting must be read explicitly; a table cannot enable it.
func TestCheckoutRequiresExplicitConfiguration(t *testing.T) {
	old := CloudFileCheckoutEnabled
	defer func() { CloudFileCheckoutEnabled = old }()
	for _, tc := range []struct {
		value string
		want  bool
	}{{"", false}, {"false", false}, {"true", true}} {
		dir := t.TempDir()
		config := "[fileserver]\nport = 8082\n[cloudfile]\n"
		if tc.value != "" {
			config += "checkout_enabled = " + tc.value + "\n"
		}
		if err := os.WriteFile(filepath.Join(dir, "seafile.conf"), []byte(config), 0600); err != nil {
			t.Fatal(err)
		}
		LoadFileServerOptions(dir)
		if CloudFileCheckoutEnabled != tc.want {
			t.Fatalf("setting %q: got %v", tc.value, CloudFileCheckoutEnabled)
		}
	}
}
