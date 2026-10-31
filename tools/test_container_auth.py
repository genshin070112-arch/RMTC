#!/usr/bin/env python3
# 文件：tools/test_container_auth.py
# 作用：人工离线验证真实容器读取 set_token.sh 默认位置的文件并发送 Bearer 认证。
# 本地 HTTP 仅返回固定合法空事件；另用 C++ ROS 探针供给感知，不访问真实模型。
# 用法：ROS_DOMAIN_ID=111 python3 tools/test_container_auth.py [新证据目录]。
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(sys.argv[1] if len(sys.argv)>1 else ROOT/'evidence/second_report_container_auth').resolve()
OUT.mkdir(parents=True, exist_ok=True)
assert not any(OUT.iterdir()), 'use a new evidence directory'
requests = []
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        snapshot = json.loads(body['messages'][1]['content'])
        ok = self.headers.get('Authorization') == 'Bearer offline-file-token'
        requests.append({'session_id':snapshot['session_id'], 'authenticated':ok})
        facts = dict(session_id=snapshot['session_id'],events=[],effects=[],goal=None)
        data = json.dumps({'choices':[{'message':{'content':json.dumps(facts)}}]}).encode()
        self.send_response(200 if ok else 401)
        self.send_header('Content-Length',str(len(data)))
        self.end_headers()
        self.wfile.write(data)
server = http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread = threading.Thread(target=server.serve_forever,daemon=True); thread.start()
name = 'rmtc-auth-fixture-' + uuid.uuid4().hex[:10]
docker = shutil.which('docker'); assert docker
with tempfile.TemporaryDirectory(prefix='rmtc-auth-fixture-') as directory:
    root = Path(directory)
    shutil.copy2(ROOT/'run_container.sh',root/'run_container.sh')
    shutil.copytree(ROOT/'config',root/'config')
    (root/'.secrets').mkdir()
    token = root/'.secrets/team.token'; token.write_text('offline-file-token\n'); token.chmod(0o600)
    # 仅为回收本测试的容器追加独立名字，实际启动参数仍由待测脚本构造。
    shim = root/'docker'
    shim.write_text('#!/usr/bin/env python3\nimport os,sys\nos.execv('+repr(docker)+', ['+repr(docker)+',sys.argv[1],"--name",'+repr(name)+']+sys.argv[2:])\n')
    shim.chmod(0o755)
    env = {k:v for k,v in os.environ.items() if k not in ('RMTC_TOKEN_FILE','RMTC_TEAM_TOKEN')}
    env.update(PATH=str(root)+':'+env['PATH'], ROS_DOMAIN_ID=env.get('ROS_DOMAIN_ID','111'),
               ROS_LOCALHOST_ONLY='0',RMW_IMPLEMENTATION='rmw_fastrtps_cpp', ROS_LOG_DIR=str(OUT/'probe_ros'))
    with (OUT/'node.log').open('w') as log:
        process = subprocess.Popen(['bash',str(root/'run_container.sh'),'--stage','2','--seat','A',
            '--gateway','http://127.0.0.1:'+str(server.server_port),'--decision-file','/app/config/decision_config.json',
            '--log-dir','/app/logs'],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        try:
            time.sleep(1)
            with (OUT/'probe.log').open('w') as output:
                subprocess.run([str(ROOT/'build/ros_fault_probe')], env=env,stdout=output,stderr=subprocess.STDOUT,timeout=35,check=True)
        finally:
            subprocess.run([docker,'stop','-t','3',name],capture_output=True,timeout=10)
            process.wait(timeout=10)
            server.shutdown(); thread.join()
    shutil.copytree(root/'logs/container',OUT/'container_logs')
rows=[]
for path in (OUT/'container_logs').glob('*.jsonl'):
    rows += [json.loads(line) for line in path.read_text().splitlines()]
accepted=[r for r in rows if r.get('kind')=='llm_result' and r.get('accepted')]
summary={'http_requests':requests,'accepted_results':len(accepted),
         'no_environment_token':True,'default_token_file':True,'real_llm_verified':False}
(OUT/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2))
assert requests and all(r['authenticated'] for r in requests), summary
assert accepted and all(r.get('status')==200 for r in accepted), summary
print('PASS real_container_default_file_authenticated_and_results_accepted',len(requests),len(accepted))
