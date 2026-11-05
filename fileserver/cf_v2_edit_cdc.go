package main

// V2 CDC reuses only blocks from the authorized current file. Other blocks
// need a scoped receipt proving this actor uploaded the bytes. There is no
// library-wide hash oracle, server session table or alternative commit path.
import (
	"crypto/hmac"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"strconv"

	"github.com/haiwen/seafile-server/fileserver/blockmgr"
	"github.com/haiwen/seafile-server/fileserver/fsmgr"
	"github.com/haiwen/seafile-server/fileserver/option"
	"github.com/haiwen/seafile-server/fileserver/repomgr"
)

const cfCDCMaxBlock int64 = 10 << 20
const cfCDCMaxBlocks = 256 // > 1 GiB / native 6 MiB minimum, with a final short block.
type cfCDCBlock struct {
	ID      string `json:"id"`
	Size    int64  `json:"size"`
	Receipt string `json:"receipt,omitempty"`
}
type cfCDCManifest struct {
	Base   string       `json:"base"`
	Size   int64        `json:"size"`
	Digest string       `json:"sha256"`
	GC     string       `json:"gc"`
	Blocks []cfCDCBlock `json:"blocks"`
}

func cfCDCValidate(m *cfCDCManifest) error {
	if !cfEditHex(m.Base, 40) || !cfEditHex(m.Digest, 64) || m.Size <= 0 || m.Size > cfEditMaxSize || len(m.GC) > 128 || len(m.Blocks) == 0 || len(m.Blocks) > cfCDCMaxBlocks {
		return errors.New("invalid manifest")
	}
	var size int64
	for _, b := range m.Blocks {
		if !cfEditHex(b.ID, 40) || b.Size <= 0 || b.Size > cfCDCMaxBlock || len(b.Receipt) > 64 {
			return errors.New("invalid block")
		}
		size += b.Size
	}
	if size != m.Size {
		return errors.New("size mismatch")
	}
	return nil
}
func cfCDCDecode(r *http.Request) (*cfCDCManifest, error) {
	if r.Method != "POST" {
		return nil, errors.New("method")
	}
	d := json.NewDecoder(io.LimitReader(r.Body, 128<<10))
	d.DisallowUnknownFields()
	m := new(cfCDCManifest)
	if e := d.Decode(m); e != nil {
		return nil, e
	}
	var tail interface{}
	if d.Decode(&tail) != io.EOF {
		return nil, errors.New("trailing input")
	}
	return m, cfCDCValidate(m)
}

// Receipts carry no authorization themselves. Every request rechecks the V2
// ticket and path permission; binding to GC prevents resurrecting removed data.
func cfCDCReceipt(c *cfEditClaims, base, gc string, b cfCDCBlock) string {
	payload, _ := json.Marshal([]interface{}{"cf-edit-cdc-v1", c.Subject, c.UID, c.RepoID, c.FilePath, c.Mode, base, gc, b.ID, b.Size})
	h := hmac.New(sha256.New, []byte(option.JWTPrivateKey))
	h.Write(payload)
	return hex.EncodeToString(h.Sum(nil))
}
func cfCDCReceiptValid(c *cfEditClaims, base, gc string, b cfCDCBlock) bool {
	return b.Receipt != "" && hmac.Equal([]byte(b.Receipt), []byte(cfCDCReceipt(c, base, gc, b)))
}
func cfCDCBaseline(repo *repomgr.Repo, c *cfEditClaims, base string) (map[string]bool, error) {
	id, _, err := cfEditActual(repo, repo.HeadCommitID, c.FilePath)
	if err != nil || id != base {
		return nil, errors.New("version changed")
	}
	f, err := fsmgr.GetSeafile(repo.StoreID, id)
	if err != nil {
		return nil, err
	}
	ids := make(map[string]bool, len(f.BlkIDs))
	for _, id := range f.BlkIDs {
		ids[id] = true
	}
	return ids, nil
}
func cfCDCPlan(w http.ResponseWriter, r *http.Request, c *cfEditClaims, repo *repomgr.Repo) {
	m, e := cfCDCDecode(r)
	if e != nil {
		cfEditFail(w, 400, "INVALID_MANIFEST")
		return
	}
	base, e := cfCDCBaseline(repo, c, m.Base)
	if e != nil {
		cfEditFail(w, 409, "VERSION_CHANGED")
		return
	}
	gc, e := repomgr.GetCurrentGCID(repo.StoreID)
	if e != nil {
		cfEditFail(w, 503, "GC_UNAVAILABLE")
		return
	}
	missing := cfCDCMissing(c, m, base, gc, func(id string) (int64, error) { return blockmgr.Stat(repo.StoreID, id) })
	cfEditJSON(w, 200, map[string]interface{}{"gc": gc, "missing": missing})
}

// Keep the authorization-before-existence rule explicit and independently testable.
func cfCDCMissing(c *cfEditClaims, m *cfCDCManifest, base map[string]bool, gc string, stat func(string) (int64, error)) []int {
	missing := make([]int, 0)
	for i, b := range m.Blocks {
		// Never query existence of an arbitrary hash unless its receipt is valid.
		if !base[b.ID] && !cfCDCReceiptValid(c, m.Base, gc, b) {
			missing = append(missing, i)
			continue
		}
		n, e := stat(b.ID)
		if e != nil || n != b.Size {
			missing = append(missing, i)
		}
	}
	return missing
}

func cfCDCBlockUpload(w http.ResponseWriter, r *http.Request, c *cfEditClaims, repo *repomgr.Repo) {
	b := cfCDCBlock{ID: r.Header.Get("X-CF-Block-ID"), Size: r.ContentLength}
	base, gc := r.Header.Get("X-CF-Base-ID"), r.Header.Get("X-CF-GC-ID")
	if r.Method != "POST" || !cfEditHex(b.ID, 40) || !cfEditHex(base, 40) || b.Size <= 0 || b.Size > cfCDCMaxBlock || len(gc) > 128 {
		cfEditFail(w, 400, "INVALID_BLOCK")
		return
	}
	if _, e := cfCDCBaseline(repo, c, base); e != nil {
		cfEditFail(w, 409, "VERSION_CHANGED")
		return
	}
	current, e := repomgr.GetCurrentGCID(repo.StoreID)
	if e != nil || gc != current {
		cfEditFail(w, 409, "GC_CHANGED")
		return
	}
	tmp, e := os.CreateTemp("", "cf-cdc-*")
	if e != nil {
		cfEditFail(w, 503, "IO_ERROR")
		return
	}
	defer os.Remove(tmp.Name())
	defer tmp.Close()
	h := sha1.New()
	n, e := io.Copy(io.MultiWriter(tmp, h), io.LimitReader(r.Body, b.Size+1))
	if e != nil || n != b.Size || hex.EncodeToString(h.Sum(nil)) != b.ID {
		cfEditFail(w, 400, "BLOCK_MISMATCH")
		return
	}
	if _, e = tmp.Seek(0, io.SeekStart); e == nil {
		e = blockmgr.Write(repo.StoreID, b.ID, tmp)
	}
	if e != nil {
		cfEditFail(w, 503, "BLOCK_UNAVAILABLE")
		return
	}
	cfEditJSON(w, 200, map[string]string{"receipt": cfCDCReceipt(c, base, gc, b)})
}
func cfCDCPublish(w http.ResponseWriter, r *http.Request, c *cfEditClaims, repo *repomgr.Repo) {
	m, e := cfCDCDecode(r)
	if e != nil {
		cfEditFail(w, 400, "INVALID_MANIFEST")
		return
	}
	action := r.Header.Get("X-CF-Action")
	if (action != "optimistic" && action != "commit" && action != "checkin") || (c.Mode == "optimistic-edit") != (action == "optimistic") {
		cfEditFail(w, 403, "MODE")
		return
	}
	proof := cfEditProof{GuardID: r.Header.Get("X-CF-Guard-ID"), Proof: r.Header.Get("X-CF-Guard-Proof")}
	proof.Generation, _ = strconv.ParseUint(r.Header.Get("X-CF-Guard-Generation"), 10, 64)
	base, e := cfCDCBaseline(repo, c, m.Base)
	if e != nil {
		cfEditFail(w, 409, "VERSION_CHANGED")
		return
	}
	gc, e := repomgr.GetCurrentGCID(repo.StoreID)
	if e != nil || gc != m.GC {
		cfEditFail(w, 409, "GC_CHANGED")
		return
	}
	h := sha256.New()
	ids := make([]string, 0, len(m.Blocks))
	for _, b := range m.Blocks {
		if !base[b.ID] && !cfCDCReceiptValid(c, m.Base, gc, b) {
			cfEditFail(w, 403, "BLOCK_PROOF_REQUIRED")
			return
		}
		n, e := blockmgr.Stat(repo.StoreID, b.ID)
		if e != nil || n != b.Size {
			cfEditFail(w, 409, "BLOCK_MISSING")
			return
		}
		// Bound and verify each block while hashing the ordered file, including repeats.
		bh := sha1.New()
		bounded := &cfCDCBoundedWriter{w: io.MultiWriter(h, bh), remaining: b.Size}
		if e = blockmgr.Read(repo.StoreID, b.ID, bounded); e != nil || bounded.remaining != 0 || hex.EncodeToString(bh.Sum(nil)) != b.ID {
			cfEditFail(w, 503, "BLOCK_MISMATCH")
			return
		}
		ids = append(ids, b.ID)
	}
	if hex.EncodeToString(h.Sum(nil)) != m.Digest {
		cfEditFail(w, 400, "CONTENT_MISMATCH")
		return
	}
	f, e := fsmgr.NewSeafile(repo.Version, m.Size, ids)
	if e == nil {
		e = fsmgr.SaveSeafile(repo.StoreID, f)
	}
	if e != nil {
		cfEditFail(w, 503, "NATIVE_FS")
		return
	}
	cfEditCommit(w, r, c, repo, action, m.Base, proof, f.FileID, m.Size, gc)
}

type cfCDCBoundedWriter struct {
	w         io.Writer
	remaining int64
}

func (b *cfCDCBoundedWriter) Write(p []byte) (int, error) {
	if int64(len(p)) > b.remaining {
		return 0, errors.New("block exceeds declared size")
	}
	n, e := b.w.Write(p)
	b.remaining -= int64(n)
	return n, e
}
