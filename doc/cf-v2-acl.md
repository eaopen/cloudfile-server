# Native CF-ACL Integration

Baseline: CE14 `b6179eb3d73b04c67a00412ebaf964153c4ad2c0`, with the frozen native
CloudFile overlay `89f645f1e46a8b2d3d05e813a3a18aa980bc2735` as this branch's parent.
Hub baseline: `64ed374bc8ceeb7d610baf9f3a99c6a60cffa93f`.

Directory ACL configuration supports exactly `r`, `rw`, and `invisible`.
`none` is not a stored V2 rule. The existing CE repo/share permissions are
unchanged; native repository `admin` shares already carry `rw` content
permission plus a management marker. This DDL contract applies to **fresh**
V2 tables; do not auto-upgrade existing populated ACL tables or silently convert
an existing deny rule on service restart. DEV-era obsolete `none` rows require
explicit reviewed cleanup before enabling protected publication.

See the matching Hub `cloudfile_ext/v2/ACL_CONTRACT.md` for the shared policy and
wire contract. The authoritative algorithm and bounded library rule cache remain in Hub. Existing v0.9 design
discussions are not implementation or acceptance evidence.

`[cloudfile] v2_acl_enabled=true` enables the minimal native integration in both C
and Go. Invalid values fail closed. The default preserves native behavior. Public
rule publication in Hub remains blocked independently of this option.

- Current-path file reads are authorized by Hub `check-access`, which applies the
  same authority before bytes/Range/304. Go does not repeat that round trip, so
  Hub and Server must come from the matching source-set pins.
- Upload/overwrite actors originate in native request tokens or trusted RPC
  arguments. Final native tree diffs are checked after locking Branch(master).
- Hub rule mutation must take that same lock first. A previous ticket alone is insufficient; Hub may evaluate
  its cached library rules (180 seconds by default, configurable 0–300 seconds;
  rule writes through Hub invalidate them on commit). A failed check rolls back the native publication.
- C post/put of fresh bytes have explicit actor context. C unscoped commits, native
  sync, public links and object-ID-only tickets cannot access protected repos.
- Copy/move is disabled globally in ACL mode until source/destination proof is
  implemented. Otherwise a copy into an unprotected destination could export
  protected source data, including when rules appear during a copy. Other
  structural operations in protected repos remain blocked.
- CF-EDIT hooks are independent and retain their separate configuration/behavior.

The source-set integrator must explicitly add the new `common/cf-v2-acl.[ch]` and
`fileserver/cf_v2_acl*.go` overlay files, plus changed native patches:
`common/Makefile.am`, `common/branch-mgr.[ch]`, `server/Makefile.am`,
`server/repo-op.c`, `server/http-server.c`, `server/http-tx-mgr.c`,
`server/web-accesstoken-mgr.c`, `fileserver/fileop.go`,
`fileserver/sync_api.go`, `fileserver/option/option.go`.
Hub also patches `seahub/urls.py` and
`seahub/api2/endpoints/internal_api.py`. No source SHA pin, live database,
container or image tag is changed by this branch.

Targeted Go tests: `go test . -run '^TestCFACL' -v` from `fileserver`.
The opt-in `tests/test_cf_v2_acl_native.py` starts disposable real C/Go services
and uses a disposable MariaDB schema plus the matching Hub policy, SQL store and
internal DRF decision endpoint. Login/subject projection are explicit test doubles.
It is not an IdP, TTL, Etech or production acceptance test. Run only in an isolated
container with `CF_ACL_NATIVE_SOURCE`, `CF_ACL_HUB_SOURCE`, `CF_ACL_C_BINARY`,
`CF_ACL_GO_BINARY`, and an isolated MariaDB reachable as `CF_ACL_DB_HOST`
(default `db`, test-only root password `acl-test-only`).
