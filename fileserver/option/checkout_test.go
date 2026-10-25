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

func TestCheckoutRepoPolicyIsOptIn(t *testing.T) {
	oldOn, oldRepos := CloudFileCheckoutEnabled, CloudFileCheckoutRepos
	defer func() { CloudFileCheckoutEnabled, CloudFileCheckoutRepos = oldOn, oldRepos }()
	const selected = "11111111-1111-4111-8111-111111111111"
	const other = "22222222-2222-4222-8222-222222222222"
	for _, tc := range []struct{ enabled, ids string; selected, other bool }{
		{"false", selected, false, false},
		{"true", "", false, false},
		{"true", selected, true, false},
	} {
		dir := t.TempDir()
		config := "[fileserver]\nport = 8082\n[cloudfile]\ncheckout_enabled = " + tc.enabled + "\ncheckout_repo_ids = " + tc.ids + "\n"
		if err := os.WriteFile(filepath.Join(dir, "seafile.conf"), []byte(config), 0600); err != nil { t.Fatal(err) }
		LoadFileServerOptions(dir)
		if CheckoutEnabledForRepo(selected) != tc.selected || CheckoutEnabledForRepo(other) != tc.other {
			t.Fatalf("enabled=%s ids=%q: wrong per-repo behavior", tc.enabled, tc.ids)
		}
	}
}

func TestCheckoutRepoListRejectsMalformed(t *testing.T) {
	for _, raw := range []string{"not-a-uuid", "11111111-1111-4111-8111-111111111111,", "11111111-1111-4111-8111-111111111111,11111111-1111-4111-8111-111111111111", "AAAAAAAA-AAAA-4AAA-AAAA-AAAAAAAAAAAA"} {
		if _, err := parseCheckoutRepoIDs(raw); err == nil { t.Fatalf("accepted invalid list %q", raw) }
	}
}
