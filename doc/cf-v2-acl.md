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

CloudFile V2 directory ACL is an always-on built-in feature in both the C
server and Go Fileserver. There is no `v2_acl_enabled` configuration key.
Native checks look up whether a library has V2 directory ACL rows (including
origin scope where applicable); SQL/subject/authority failures deny access,
and libraries without directory ACL rules continue to use native CE grants.
Protected repositories still require current Hub file-path authorization
and scoped final-commit checks. Legacy unscoped structural transfers remain
restricted until their source/destination proof is implemented.
Hub's ACL publication has no independent enable toggle, but still requires
authenticated native repository management rights and revision CAS.

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
- Copy/move works only between two non-virtual libraries confirmed to have no
  ACL rules. Both library transfer fences are acquired in repository-ID order
  on a dedicated connection and held through synchronous publication. Hub ACL
  writes take the same fence before Branch(master), so a new source deny cannot
  race an ordinary copy into an unprotected destination. Fence/schema failure
  and protected/virtual scopes still deny. Requested background transfers run
  synchronously until the background worker can carry the same proof safely.
  Other structural operations in protected repos remain blocked.
- CF-EDIT native Checkout Guard is independent of ACL but is no longer controlled by an ENABLE option: native processes detect the installed `cf_edit_guard` schema. If present, it must be enforced on every relevant final write; schema/SQL failure denies, not bypasses. Installing Guard in an existing deployment requires a controlled schema migration and restart.

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
