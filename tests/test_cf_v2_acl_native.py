"""Disposable C/Go byte + final-CAS ACL test.

Needs built binaries, disposable MariaDB and Hub checkout. Uses real Hub policy,
SQL store and internal DRF decision endpoint. Login/subject TTL are test adapters;
this is NOT an IdP, Etech or production deployment acceptance test.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch
from uuid import uuid4



def run():
    # This executable acceptance harness needs its own DB/JWT dependencies;
    # ordinary upstream pytest collection must not require or start them.
    import jwt
    import pymysql
    import requests
    source = Path(os.environ['CF_ACL_NATIVE_SOURCE'])
    hub = Path(os.environ['CF_ACL_HUB_SOURCE'])
    binary = Path(os.environ['CF_ACL_C_BINARY'])
    go_binary = Path(os.environ['CF_ACL_GO_BINARY'])
    options = dict(host=os.environ.get('CF_ACL_DB_HOST','db'),user='root',password=os.environ.get('CF_ACL_DB_PASSWORD','acl-test-only'),autocommit=True,charset='utf8mb4')
    admin = pymysql.connect(**options)
    schemas = {kind:'acl_native_'+uuid4().hex for kind in ('account','files')}
    processes=[]
    server=None
    key='isolated-native-acl-proof-not-a-deployment-secret-123'
    checks=[]
    try:
        with admin.cursor() as c:
            for kind,name in schemas.items():
                c.execute('CREATE DATABASE '+name)
                c.execute('USE '+name)
                file=source/'scripts/sql/mysql'/('ccnet.sql' if kind=='account' else 'seafile.sql')
                for sql in re.findall(r'CREATE TABLE[\s\S]+?;',file.read_text()):
                    c.execute(sql)
        options['database']=schemas['files']
        from cloudfile_ext.v2.schema import FreshSchema
        conn=pymysql.connect(**options)
        FreshSchema(conn,(source/'scripts/sql/mysql/cloudfile-v2.sql').read_text()).install()
        with tempfile.TemporaryDirectory(prefix='cf-acl-native-') as temp:
            root=Path(temp)
            conf,data,ccnet=(root/name for name in ('conf','data','ccnet'))
            for directory in (conf,data,ccnet):directory.mkdir()
            (conf/'seafile.conf').write_text(f'''[database]
type=mysql
host={options['host']}
user=root
password={options['password']}
db_name={schemas['files']}
ccnet_db_name={schemas['account']}
connection_charset=utf8mb4
[fileserver]
use_go_fileserver=true
port=8082
''')
            os.environ.update(SEAFILE_DATA_DIR=str(data),SEAFILE_CENTRAL_CONF_DIR=str(conf),SEAHUB_LOG_DIR=str(root),
                SEAFILE_MYSQL_DB_HOST=options['host'],SEAFILE_MYSQL_DB_USER='root',SEAFILE_MYSQL_DB_PASSWORD=options['password'],
                SEAFILE_MYSQL_DB_CCNET_DB_NAME=schemas['account'],SEAFILE_MYSQL_DB_SEAFILE_DB_NAME=schemas['files'],
                JWT_PRIVATE_KEY=key,ENABLE_GO_FILESERVER='true',DJANGO_SETTINGS_MODULE='seahub.settings')
            for label,cmd in (
                ('c',[str(binary),'-F',str(conf),'-c',str(ccnet),'-d',str(data),'-l',str(root/'c.log'),'-f']),
                ('go',[str(go_binary),'-F',str(conf),'-d',str(data),'-l',str(root/'go.log')])):
                processes.append(subprocess.Popen(cmd,stdout=(root/(label+'.stderr')).open('w'),stderr=subprocess.STDOUT))
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                if (data/'seafile.sock').exists():
                    try:
                        if requests.get('http://127.0.0.1:8082/protocol-version',timeout=1).status_code==200:break
                    except requests.RequestException:pass
                time.sleep(.1)
            else:
                raise AssertionError({p.name:p.read_text()[-4000:] for p in root.glob('*.log')})
            import django
            django.setup()
            from django.conf import settings
            settings.CF_ACL_CACHE_TTL = 0  # strict mode proves the commit seam independently
            from seaserv import ccnet_api,seafile_api
            from rest_framework.test import APIRequestFactory
            from cloudfile_ext.v2.acl_policy import Principal,Rule
            from cloudfile_ext.v2.acl_store import ACLStore
            from cloudfile_ext.v2.acl_runtime import Authority
            from cloudfile_ext.v2.acl_http import NativeACLCheck
            owner,member='owner@acl.test','member@acl.test'
            for username in (owner,member):assert ccnet_api.add_emailuser(username,'!',0,1)==0
            repo=seafile_api.create_repo('ACL isolated byte test','',owner)
            assert seafile_api.post_dir(repo,'/','shared',owner)==0
            assert seafile_api.post_dir(repo,'/','sibling',owner)==0
            assert seafile_api.check_permission(repo,member) is None
            @contextmanager
            def atomic():
                conn.begin()
                try:yield;conn.commit()
                finally:conn.rollback()
            store=ACLStore(conn,schemas['files'],atomic)
            # Real native transfers must work without rules, then reject a
            # source ACL installed while the transfer waits for its fence.
            from concurrent.futures import ThreadPoolExecutor
            from hashlib import sha256
            plain=seafile_api.create_repo('Native ordinary transfer','',owner)
            target=seafile_api.create_repo('Native transfer destination','',owner)
            local=root/'plain.txt';local.write_bytes(b'native ACL transfer proof')
            assert seafile_api.post_file(plain,str(local),'/', 'plain.txt',owner)==0
            seafile_api.copy_file(plain,'/',json.dumps(['plain.txt']),target,'/',json.dumps(['copy.txt']),owner,0,synchronous=0)
            assert seafile_api.get_file_id_by_path(target,'/copy.txt')
            seafile_api.move_file(target,'/',json.dumps(['copy.txt']),plain,'/',json.dumps(['moved.txt']),0,owner,0,synchronous=0)
            assert seafile_api.get_file_id_by_path(plain,'/moved.txt')
            assert not seafile_api.get_file_id_by_path(target,'/copy.txt')
            checks.append('Ordinary copy/move works synchronously without ACL rules')
            fence='cfv2:t:'+sha256((schemas['files']+':'+plain).encode()).hexdigest()[:56]
            with conn.cursor() as cursor:
                cursor.execute('SELECT GET_LOCK(%s,5)',[fence]);assert cursor.fetchone()==(1,)
            try:
                with ThreadPoolExecutor(max_workers=1) as pool:
                    def waiting_copy():
                        try:
                            seafile_api.copy_file(plain,'/',json.dumps(['plain.txt']),target,'/',json.dumps(['escape.txt']),owner,0,synchronous=1)
                            return True
                        except Exception:
                            return False
                    pending=pool.submit(waiting_copy)
                    try:
                        time.sleep(.2)
                        assert not pending.done(), 'native transfer bypassed the ACL mutation fence'
                        restriction=Rule(plain,'/','etech','user','user','owner-uid','invisible',True,False)
                        restriction_revision=str(uuid4())
                        store.mutate(restriction,expected=None,revision=restriction_revision)
                    finally:
                        with conn.cursor() as cursor:
                            cursor.execute('SELECT RELEASE_LOCK(%s)',[fence]);assert cursor.fetchone()==(1,)
                    assert pending.result(timeout=10) is False
                assert not seafile_api.get_file_id_by_path(target,'/escape.txt')
                store.mutate(restriction,expected=restriction_revision,revision=str(uuid4()),delete=True)
                checks.append('Concurrent source ACL publication prevents native byte export')
            finally:
                seafile_api.remove_repo(plain)
                seafile_api.remove_repo(target)
            revision=str(uuid4())
            rule=Rule(repo,'/shared','etech','user','user','member-uid','rw',True,True)
            store.mutate(rule,expected=None,revision=revision)
            def change(permission):
                nonlocal revision,rule
                old=revision;revision=str(uuid4())
                rule=Rule(repo,'/shared','etech','user','user','member-uid',permission,True,permission in ('r','rw'))
                store.mutate(rule,expected=old,revision=revision)
            revoke_after_preflight=threading.Event()
            def authority(username):
                account=ccnet_api.get_emailuser(username)
                if not account or not account.is_active:raise PermissionError()
                connection=pymysql.connect(**options)
                # Closed after the request by Handler. Subjects come from this
                # fixed test login adapter, never request-supplied groups.
                current_connections.values.append(connection)
                return Authority(username,principal=Principal('etech','member-uid' if username==member else 'owner-uid'),
                    rules=ACLStore(connection,schemas['files'],lambda:None),api=seafile_api)
            current_connections=threading.local()
            class Handler(BaseHTTPRequestHandler):
                def log_message(self,*args):pass
                def do_POST(self):
                    current_connections.values=[]
                    try:
                        raw=self.rfile.read(int(self.headers.get('Content-Length','0')))
                        payload=json.loads(raw)
                        jwt.decode(self.headers.get('Authorization','').split()[1],key,algorithms=['HS256'])
                        if self.path.endswith('/cloudfile/acl/check/'):
                            req=APIRequestFactory().post(self.path,payload,format='json',HTTP_AUTHORIZATION=self.headers['Authorization'])
                            reply=NativeACLCheck.as_view()(req)
                            status,data_reply=reply.status_code,reply.data
                            if status==200 and revoke_after_preflight.is_set() and payload['checks'][0]['kind']=='dir':
                                change('r');revoke_after_preflight.clear()
                        elif '/check-access/' in self.path:
                            # Test-only login adapter; production uses native
                            # account token/session authentication in Hub.
                            if payload.get('token')!='member-session':raise PermissionError()
                            decision=authority(member).effective(repo,'/'+payload['path'].lstrip('/'),'file')
                            status,data_reply=(200,{'user':member}) if decision.can_read else (403,{})
                        else:status,data_reply=404,{}
                    except Exception as e:
                        status,data_reply=503,{'error':type(e).__name__}
                    finally:
                        for connection in current_connections.values:connection.close()
                    self.send_response(status);self.send_header('Content-Type','application/json');self.end_headers()
                    self.wfile.write(json.dumps(data_reply).encode())
            with patch('cloudfile_ext.v2.acl_http.Authority',side_effect=authority),patch('cloudfile_ext.v2.acl_http.JWT_PRIVATE_KEY',key):
                server=ThreadingHTTPServer(('127.0.0.1',8000),Handler)
                threading.Thread(target=server.serve_forever,daemon=True).start()
                content=b'Current directory ACL byte proof\n'*2048
                def upload(name,body=content):
                    token=seafile_api.get_fileserver_access_token(repo,json.dumps({'parent_dir':'/shared'}),'upload',member,False)
                    return requests.post('http://127.0.0.1:8082/upload-api/'+token,data={'parent_dir':'/shared'},files={'file':(name,body)},timeout=15)
                reply=upload('proof.txt')
                assert reply.status_code==200,(reply.status_code,reply.text,(root/'go.log').read_text()[-3000:])
                checks.append('Go upload + final commit: directory-only rw')
                shared_view=Authority(member,principal=Principal('etech','member-uid'),rules=store,api=seafile_api)
                listed=shared_view.entries(repo,'/shared')
                assert listed['root_path']=='/shared' and listed['data'][0]['name']=='proof.txt'
                for hidden in ('/','/sibling'):
                    try:shared_view.entries(repo,hidden)
                    except PermissionError:pass
                    else:raise AssertionError('escaped shared root')
                library_view=Authority(owner,principal=Principal('etech','owner-uid'),rules=store,api=seafile_api)
                assert library_view.entries(repo,'/')['root_path']=='/'
                checks.append('Shared-root view versus native-library root uses identical authority')
                url=f'http://127.0.0.1:8082/repos/{repo}/files/shared/proof.txt?op=download'
                headers={'Authorization':'Token member-session'}
                reply=requests.get(url,headers=headers,timeout=15)
                assert reply.status_code==200 and reply.content==content,(reply.status_code,reply.text[:200])
                reply=requests.get(url,headers=dict(headers,Range='bytes=3-27'),timeout=15)
                assert reply.status_code==206 and reply.content==content[3:28]
                checks.append('Go current-path read + Range without library membership')
                file_id=seafile_api.get_file_id_by_path(repo,'/shared/proof.txt')
                token=seafile_api.get_fileserver_access_token(repo,file_id,'download',member,False)
                assert requests.get('http://127.0.0.1:8082/files/'+token+'/proof.txt',timeout=10).status_code!=200
                checks.append('Legacy object-only ticket blocked')
                local=root/'c-file';local.write_bytes(content)
                assert seafile_api.post_file(repo,str(local),'/shared','c-proof.txt',member)==0
                assert seafile_api.get_file_id_by_path(repo,'/shared/c-proof.txt')
                checks.append('C fresh byte upload + final commit: same Hub authority')
                local.write_bytes(b'C overwrite under same policy')
                assert seafile_api.put_file(repo,str(local),'/shared','c-proof.txt',member,None)
                checks.append('C overwrite under final path authority')
                # Copying unread source bytes into an unprotected destination
                # must not bypass the closed source-side integration boundary.
                destination=seafile_api.create_repo('Unprotected destination','',member)
                destination_head=seafile_api.get_repo(destination).head_cmmt_id
                try:
                    seafile_api.copy_file(repo,'/shared',json.dumps(['proof.txt']),destination,'/',json.dumps(['escape.txt']),member,0,synchronous=1)
                except Exception:pass
                assert seafile_api.get_repo(destination).head_cmmt_id==destination_head
                assert not seafile_api.get_file_id_by_path(destination,'/escape.txt')
                checks.append('Cross-repository copy cannot export protected bytes')
                head=seafile_api.get_repo(repo).head_cmmt_id
                revoke_after_preflight.set()
                reply=upload('revoked-after-preflight.txt')
                assert reply.status_code!=200,(reply.status_code,reply.text)
                assert seafile_api.get_repo(repo).head_cmmt_id==head
                assert not seafile_api.get_file_id_by_path(repo,'/shared/revoked-after-preflight.txt')
                checks.append('Go preflight rw -> r revocation blocks final Branch CAS')
                try:seafile_api.post_file(repo,str(local),'/shared','c-denied.txt',member)
                except Exception:pass
                assert not seafile_api.get_file_id_by_path(repo,'/shared/c-denied.txt')
                assert seafile_api.get_repo(repo).head_cmmt_id==head
                checks.append('C current r denies byte publication')
                change('invisible')
                assert requests.get(url,headers=dict(headers,Range='bytes=3-27'),timeout=15).status_code==403
                checks.append('Current deny revokes path/Range read')
                change('rw')
                assert upload('after-denial.txt').status_code==200
                checks.append('Denied transaction releases Branch lock')
                head=seafile_api.get_repo(repo).head_cmmt_id
                try:seafile_api.rename_file(repo,'/shared','proof.txt','moved.txt',member)
                except Exception:pass
                assert seafile_api.get_repo(repo).head_cmmt_id==head
                checks.append('Unscoped structural C operation remains closed')
                from django.test import override_settings
                from django.core.cache import cache
                # LocMem cannot invalidate other workers. Production authority
                # disables cross-request grants for this backend, even with TTL.
                with override_settings(CF_ACL_CACHE_TTL=2,CACHES={'default':{'BACKEND':'django.core.cache.backends.locmem.LocMemCache','LOCATION':'native-acl-proof'}}):
                    cache.clear()
                    assert upload('cache-warm.txt').status_code==200
                    change('r')
                    cached_head=seafile_api.get_repo(repo).head_cmmt_id
                    assert upload('cache-revoked.txt').status_code!=200
                    try:seafile_api.post_file(repo,str(local),'/shared','c-cache-revoked.txt',member)
                    except Exception:pass
                    assert not seafile_api.get_file_id_by_path(repo,'/shared/c-cache-revoked.txt')
                    assert seafile_api.get_repo(repo).head_cmmt_id==cached_head
                    checks.append('Local cache cannot retain C/Go grants after revocation')
                with conn.cursor() as cursor:cursor.execute('RENAME TABLE cf_dir_acl TO acl_temporarily_unavailable')
                try:
                    unavailable_head=seafile_api.get_repo(repo).head_cmmt_id
                    assert upload('sql-unavailable.txt').status_code!=200
                    try:seafile_api.post_file(repo,str(local),'/shared','c-sql-unavailable.txt',member)
                    except Exception:pass
                    assert seafile_api.get_repo(repo).head_cmmt_id==unavailable_head
                    checks.append('C/Go authoritative SQL outage denies publication on a cache miss')
                finally:
                    with conn.cursor() as cursor:cursor.execute('RENAME TABLE acl_temporarily_unavailable TO cf_dir_acl')
            conn.close()
            print(json.dumps({'passed':checks,'subject_login_adapter':'test double; real IdP/TTL not certified'},indent=2))
    finally:
        if server:server.shutdown();server.server_close()
        for process in reversed(processes):
            process.terminate()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        with admin.cursor() as c:
            for name in schemas.values():c.execute('DROP DATABASE IF EXISTS '+name)
        admin.close()


if __name__=='__main__':run()
