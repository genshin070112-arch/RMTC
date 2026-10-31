#!/usr/bin/env python3
# 文件：tools/test_container_launcher.py
# 作用：人工离线验收容器启动参数；使用假的 docker 捕获参数，不读取用户真实令牌。
# 此 Python 只用于测试，不参与参赛程序；用法：python3 tools/test_container_launcher.py。
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='rmtc-launcher-test-') as folder:
    root = Path(folder)
    (root / '.secrets').mkdir()
    (root / 'config').mkdir()
    shutil.copy2(ROOT / 'run_container.sh', root / 'run_container.sh')
    capture = root / 'args.json'
    fake = root / 'docker'
    fake.write_text('#!/usr/bin/env python3\nimport json,os,sys\nopen(os.environ["RMTC_TEST_CAPTURE"],"w").write(json.dumps(sys.argv[1:]))\n')
    fake.chmod(0o755)
    base = {k: v for k, v in os.environ.items() if k not in ('RMTC_TOKEN_FILE', 'RMTC_TEAM_TOKEN')}
    base.update(PATH=str(root) + ':' + base['PATH'], RMTC_TEST_CAPTURE=str(capture))
    def run(extra=None, args=()):
        subprocess.run(['bash', str(root/'run_container.sh'), '--stage', '2', *args],
                       env={**base, **(extra or {})}, check=True)
        return json.loads(capture.read_text())
    count = 0
    def check(value, name):
        global count
        assert value, name
        count += 1
        print('PASS', name)
    check(not any(x.startswith('RMTC_TOKEN_FILE=') and x != 'RMTC_TOKEN_FILE=' for x in run()), 'no_file_does_not_invent_secret')
    (root/'.secrets/team.token').write_text('fixture-token-not-a-real-secret\n')
    check('RMTC_TOKEN_FILE=/app/.secrets/team.token' in run(), 'set_token_default_file_connected')
    args = run({'RMTC_TEAM_TOKEN':'fixture-env-token'})
    check('RMTC_TOKEN_FILE=' in args and 'RMTC_TEAM_TOKEN=fixture-env-token' in args, 'explicit_environment_token_wins_default')
    check('RMTC_TOKEN_FILE=/app/.secrets/custom.token' in run({'RMTC_TOKEN_FILE':'/app/.secrets/custom.token'}), 'explicit_file_preserved')
    args = run(args=('--token-file', '/app/.secrets/override.token'))
    check(args[-2:] == ['--token-file','/app/.secrets/override.token'], 'cli_override_forwarded')
    print(f'RESULT {count} passed, 0 failed')
