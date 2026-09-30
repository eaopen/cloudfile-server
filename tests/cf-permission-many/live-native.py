"""Isolated seaf-server + MySQL integration, never run against a deployment.

Requires the private /lab fixture created by run-native.sh. Uses real native
writers/RPC/storage and production Search orchestration/snapshot logic. Only
Seahub boot/authentication and ORM wiring are replaced with fixture adapters.
Test-only C log counters are optional; they are not production instrumentation.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
import threading
import time
from uuid import uuid4
from types import ModuleType, SimpleNamespace
from unittest.mock import Mock, patch

if os.environ.get('CF_PERMISSION_MANY_ISOLATED') != '1':
    raise SystemExit('Refusing non-isolated native fixture')
sys.path[:0] = ['/hub', '/server/python', '/backend/seafile-server/seafile/lib/python3.12/site-packages']
import pymysql
from seafile.rpcclient import SeafServerThreadedRpcClient
from django.conf import settings
settings.configure(SECRET_KEY='isolated-test-only', DEFAULT_CHARSET='utf-8',
    CF_PROVIDER_SEARCH='meilisearch', REST_FRAMEWORK={'UNAUTHENTICATED_USER': None})
from rest_framework.renderers import JSONRenderer
from cloudfile_ext.registry import registry

DB_HOST = os.environ['CF_NATIVE_DB_HOST']
def database():
    return pymysql.connect(host=DB_HOST, user='root', password='', database='cf_lab_seafile', autocommit=True)
def client(): return SeafServerThreadedRpcClient('/lab/seafile-data/seafile.sock')
db, rpc = database(), client()
def sql(statement, args=(), connection=db):
    with connection.cursor() as cursor:
        cursor.execute(statement, args)
        return cursor.fetchall()
# This fixture targets legacy CE deployments. The shared bootstrap also creates
# modern 4A tables; preserve its ACL table under a fixture-only name, and install
# the legacy schema consumed by this unmodified scalar engine and Hub reader.
columns = {row[0] for row in sql('SHOW COLUMNS FROM cf_dir_acl')}
if 'subject' not in columns:
    sql('RENAME TABLE cf_dir_acl TO cf_dir_acl_modern_fixture')
    sql('CREATE TABLE cf_dir_acl (id BIGINT PRIMARY KEY AUTO_INCREMENT, repo_id CHAR(36) NOT NULL, '
        'path TEXT NOT NULL, path_hash CHAR(40) NOT NULL, subject_type VARCHAR(16) NOT NULL, '
        'subject VARCHAR(255) NOT NULL, permission VARCHAR(16) NOT NULL, inherit INT NOT NULL DEFAULT 1, '
        'ctime BIGINT, mtime BIGINT, UNIQUE KEY(repo_id,path_hash,subject_type,subject)) ENGINE=InnoDB')
run_id = uuid4().hex[:8]
owner, user = 'search-owner-' + run_id + '@example.test', 'search-reader-' + run_id + '@example.test'
for identity in (owner, user): rpc.add_emailuser(identity, 'fixture-unusable', 0, 1)
group = rpc.create_group('Search fixture group', owner, 'Group', -1)
rpc.group_add_member(group, owner, user)
repo = rpc.create_repo('Search transport fixture', 'Disposable integration', owner, None, 2, None, None)
rpc.add_share(repo, owner, user, 'rw')
rpc.post_dir(repo, '/', 'a', owner)
for i in range(100): rpc.post_empty_file(repo, '/a', 'drawing' + str(i), owner)
# Use the unchanged legacy ACL column/identity semantics, never 4A rules.
def reset_acl():
    sql('DELETE FROM cf_dir_acl WHERE repo_id=%s', (repo,))
    sql('INSERT INTO cf_dir_acl(repo_id,path,path_hash,subject_type,subject,permission,inherit,ctime,mtime) '
        'VALUES(%s,%s,%s,%s,%s,%s,1,0,0)', (repo, '/', hashlib.sha1(b'/').hexdigest(), 'user', user, 'rw'))
reset_acl()

transport, hook_calls, stage, mutation = 0, 0, None, None
mutated = False
writer_failures = []
def revoke(connection=None):
    global mutated
    if mutated: return
    native = client()  # Independent native writer connection/thread.
    if mutation == 'acl':
        sql("UPDATE cf_dir_acl SET permission='none' WHERE repo_id=%s", (repo,), connection or db)
    elif mutation == 'share': native.remove_share(repo, owner, user)
    elif mutation == 'group': native.group_remove_member(group, owner, user)
    elif mutation == 'account':
        account = native.get_emailuser(user)
        native.update_emailuser('DB', account.id, 'fixture-unusable', 0, 0)
    mutated = True

def reset_mutation(kind):
    global mutation, mutated, transport, hook_calls
    mutation, mutated, transport, hook_calls = kind, False, 0, 0
    reset_acl()
    account = rpc.get_emailuser(user)
    rpc.update_emailuser('DB', account.id, 'fixture-unusable', 0, 1)
    if not any(g.id == group for g in rpc.get_groups(user, 0)):
        rpc.group_add_member(group, owner, user)
    if kind == 'group':
        rpc.remove_share(repo, owner, user)
        rpc.group_share_repo(repo, group, owner, 'rw')
    else:
        rpc.group_unshare_repo(repo, group, owner)
        rpc.add_share(repo, owner, user, 'rw')

def c_counts():
    text = Path('/lab/native-counters.log').read_text()
    return {key: text.count('CF_TEST_' + key) for key in ('scalar', 'evaluate')}

def many(raw):
    global transport
    transport += 1
    if stage == 'second_pass' and transport == 3:
        # Block the first native ACL SELECT inside the real multi-check, then
        # commit a writer mutation and release it. This is test-only gating,
        # never a read/write lock added to Search or the production writers.
        gate = database()
        sql('LOCK TABLES cf_dir_acl WRITE', connection=gate)
        before = c_counts()['scalar']
        def writer():
            try:
                deadline = time.monotonic() + 5
                while c_counts()['scalar'] == before:
                    if time.monotonic() > deadline: raise RuntimeError('native scalar did not enter')
                    time.sleep(.005)
                revoke(gate if mutation == 'acl' else None)
            except Exception as error: writer_failures.append(repr(error))
            finally:
                sql('UNLOCK TABLES', connection=gate)
                gate.close()
        thread = threading.Thread(target=writer)
        thread.start()
        try: result = rpc.cf_check_permissions_many(raw)
        finally: thread.join(6)
        assert not thread.is_alive() and not writer_failures, writer_failures
    else:
        result = rpc.cf_check_permissions_many(raw)
    if stage == 'first_pass' and transport == 2 or stage == 'after_second' and transport == 3:
        revoke()
    return result

def metadata(repo_id, path):
    item = rpc.get_dirent_by_path(repo_id, path)
    if stage == 'metadata': revoke()
    return item

api = SimpleNamespace(cf_check_permissions_many=many, get_repo=rpc.get_repo,
    get_dirent_by_path=metadata, check_permission=rpc.check_permission,
    get_repo_status=rpc.get_repo_status, list_dir_by_path=rpc.list_dir_by_path)
ccnet = SimpleNamespace(get_emailuser=rpc.get_emailuser,
    get_groups=lambda username: rpc.get_groups(username, 0), get_group=rpc.get_group)
class Rules:
    def filter(self, repo_id): self.repo_id = repo_id; return self
    def values(self, *fields):
        rows = sql('SELECT ' + ','.join(fields) + ' FROM cf_dir_acl WHERE repo_id=%s LIMIT 4097', (self.repo_id,))
        return [dict(zip(fields, row)) for row in rows]
def module(name, **attributes):
    result = ModuleType(name); result.__dict__.update(attributes); return result
def load(name, path, modules):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, modules): spec.loader.exec_module(result)
    return result
snapshot = load('cloudfile_ext.search._native_snapshot', '/hub/cloudfile_ext/search/access_runtime.py', {
    'django.db': module('django.db', connection=db),
    'seaserv': module('seaserv', seafile_api=api, ccnet_api=ccnet),
    'seahub.utils.db_api': module('seahub.utils.db_api', SeafileDB=lambda: SimpleNamespace(db_name='cf_lab_seafile')),
    'cloudfile_ext.acl.models': module('cloudfile_ext.acl.models', DirACL=SimpleNamespace(objects=Rules())),
    'cloudfile_ext.features': module('cloudfile_ext.features', is_enabled=lambda flag: True),
})
view = load('cloudfile_ext.search._native_view', '/hub/cloudfile_ext/search/bounded_view.py', {
    'seaserv': module('seaserv', seafile_api=api),
    'cloudfile_ext.search.access_runtime': module('cloudfile_ext.search.access_runtime', read_snapshot=snapshot.read_snapshot),
    'seahub.api2.authentication': module('seahub.api2.authentication', TokenAuthentication=object),
    'seahub.api2.throttling': module('seahub.api2.throttling', UserRateThrottle=object),
    'seahub.utils.timeutils': module('seahub.utils.timeutils', timestamp_to_isoformat_timestr=str),
})
def hook(username, repo_id, path, permission):
    global hook_calls
    hook_calls += 1
    return permission
registry.register_permission_check(hook)

class Renderer(JSONRenderer):
    def render(self, data, *args, **kwargs):
        result = super().render(data, *args, **kwargs)
        if stage == 'serialization': revoke()
        return result

def search(count):
    index = Mock()
    index._call.return_value = dict(hits=[dict(repo_id=repo, path='/a/drawing' + str(i)) for i in range(count)])
    view.client_from_settings = lambda: index
    request = SimpleNamespace(GET=dict(repo_id=repo, path='/a', q='drawing', limit=str(count)),
        user=SimpleNamespace(username=user), accepted_renderer=Renderer(), accepted_media_type='application/json')
    instance = view.BoundedSearch()
    instance.args, instance.kwargs, instance.request, instance.format_kwarg = (), {}, request, None
    return instance.get(request)

# Record actual server log counters and executed SQL, excluding PREPARE traffic.
counts = []
for count in (1, 20, 50, 100):
    reset_mutation(None)
    sql("SET GLOBAL general_log='OFF'")
    sql('TRUNCATE TABLE mysql.general_log')
    sql("SET GLOBAL log_output='TABLE'")
    sql("SET GLOBAL general_log='ON'")
    before = c_counts()
    try:
        response = search(count)
    finally: sql("SET GLOBAL general_log='OFF'")
    assert response.status_code == 200, response.data
    assert len(response.data['data']) == count
    after = c_counts()
    queries = [row[0].decode() if isinstance(row[0], bytes) else row[0] for row in sql("SELECT argument FROM mysql.general_log WHERE command_type IN ('Execute','Query')")]
    native_acl = sum(bool(re.search(r'FROM cf_dir_acl WHERE repo_id =', query)) for query in queries)
    membership = sum(bool(re.search(r'\bGroupUser\b', query, re.I)) for query in queries)
    # Raw queries are fixture-only evidence; no production identities are used.
    Path('/lab/sql-%d.json' % count).write_text(json.dumps(queries, indent=2))
    row = dict(results=count, before_path_rpc=2*(count+2), after_path_rpc=transport,
        scalar=after['scalar']-before['scalar'], c_evaluate=after['evaluate']-before['evaluate'],
        native_acl_sql=native_acl, membership_sql=membership, hub_hooks=hook_calls,
        share_qualification_sql=sum('SELECT permission FROM SharedRepo' in q for q in queries))
    assert row['scalar'] == row['c_evaluate'] == row['native_acl_sql'] == row['hub_hooks'] == 2*(count+2), row
    # Re-run the same orchestration with a test-only scalar transport baseline.
    # Production has no fallback to this N-RPC implementation.
    baseline_transports = [0]
    factory = view.NativePermissionMany
    def scalar_factory(repo_id, username, unused_rpc, registered_hook):
        def check(paths):
            result = []
            for path in paths:
                baseline_transports[0] += 1
                permission = rpc.check_permission_by_path(repo_id, path, username)
                result.append(registered_hook(username, repo_id, path, permission)
                    if permission in ('r', 'rw') else None)
            return result
        return check
    baseline_before = c_counts()
    view.NativePermissionMany = scalar_factory
    try: baseline_response = search(count)
    finally: view.NativePermissionMany = factory
    baseline_after = c_counts()
    assert baseline_response.status_code == 200
    assert baseline_response.data['data'] == response.data['data']
    row['before_path_rpc'] = baseline_transports[0]
    row['before_scalar'] = baseline_after['scalar'] - baseline_before['scalar']
    row['before_c_evaluate'] = baseline_after['evaluate'] - baseline_before['evaluate']
    assert row['before_path_rpc'] == row['before_scalar'] == row['before_c_evaluate'] == row['scalar']
    counts.append(row)
    print(json.dumps(row), flush=True)

# Real scalar/multi parity across repository grants, exact denies and duplicates.
reset_mutation(None)
for permission in ('rw', 'none', 'invisible'):
    sql('UPDATE cf_dir_acl SET permission=%s WHERE repo_id=%s', (permission, repo))
    paths = ['/', '/a', '/a/drawing0', '/a/drawing0']
    expected = [rpc.check_permission_by_path(repo, p, user) for p in paths]
    result = json.loads(rpc.cf_check_permissions_many(json.dumps(dict(version=1, repo_id=repo, user=user, paths=paths))))
    assert [item['permission'] for item in result['items']] == [v if v in ('r', 'rw') else None for v in expected]

mutations = []
for mutation_kind in ('acl', 'share', 'group', 'account'):
    for mutation_stage in ('first_pass', 'metadata', 'second_pass', 'after_second', 'serialization'):
        reset_mutation(mutation_kind)
        stage = mutation_stage
        response = search(1)
        assert mutated, (mutation_kind, mutation_stage, response.data)
        assert response.status_code == 503 and 'data' not in response.data, (mutation_kind, mutation_stage, response.data)
        mutations.append(dict(writer=mutation_kind, stage=mutation_stage, status=response.status_code))
        print('PASS mutation', mutation_kind, mutation_stage, flush=True)
        stage = None
Path('/lab/native-search-results.json').write_text(json.dumps(dict(counts=counts, parity=True, mutations=mutations), indent=2))
print('PASS real RPC/scalar parity and 20 native mutation cases', flush=True)
