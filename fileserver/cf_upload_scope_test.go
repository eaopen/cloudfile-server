package main

import (
	"bytes"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"testing"
)

// Run the real multipart handler. No DB is installed: malformed or unbound
// destinations must be rejected before any policy lookup or object write.
func TestUploadBlksRejectsUnboundOrEscapedDestination(t *testing.T) {
	old := seafileDB
	seafileDB = nil
	defer func() { seafileDB = old }()
	for _, tc := range []struct{ token, parent, name string }{
		{"", "/", "file"},
		{"/A", "/B", "file"},
		{"/A", "/AB", "file"},
		{"/A", "/A/sub", "file"},
		{"/A", "/A/../B", "file"},
		{"/A", "/A", "../file"},
		{"/A", "/A", "sub/file"},
		{"/A", "/A", "bad\\name"},
	} {
		t.Run(tc.token+":"+tc.parent+":"+tc.name, func(t *testing.T) {
			var body bytes.Buffer
			form := multipart.NewWriter(&body)
			for key, value := range map[string]string{"parent_dir": tc.parent, "file_name": tc.name, "file_size": "1", "commitonly": "1"} {
				if err := form.WriteField(key, value); err != nil {
					t.Fatal(err)
				}
			}
			if err := form.Close(); err != nil {
				t.Fatal(err)
			}
			request := httptest.NewRequest(http.MethodPost, "/upload-blks-api/token", &body)
			request.Header.Set("Content-Type", form.FormDataContentType())
			result := doUploadBlks(httptest.NewRecorder(), request, &recvData{repoID: "11111111-1111-4111-8111-111111111111", user: "native", parentDir: tc.token})
			if result == nil || result.Code != http.StatusForbidden {
				t.Fatalf("destination escaped: %v", result)
			}
		})
	}
}
