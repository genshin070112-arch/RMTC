#!/usr/bin/env python3
# 文件：tools/verify_v11_referee.py
# 作用：人工离线验收 v1.1 练习裁判；与 C++ 预测器独立，按手算期望核对真实感知。
# 使用练习管理 API 部署夹具，调用 tests/referee_probe.cpp 控制专用测试席位并存证。
# 不是参赛策略；正式比赛不得使用多席位探针或管理 API。用法见 docs/v1.1迁移说明.md。
import argparse
import copy
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--url', default='http://127.0.0.1:18113')
parser.add_argument('--domain', default='87')
parser.add_argument('--output', type=Path, default=ROOT / 'evidence/v11_referee_cases',
                    help='新一轮存证目录，避免覆盖历史验收')
args = parser.parse_args()
OUT = args.output.resolve()
OUT.mkdir(parents=True, exist_ok=True)

def api(path, data=None):
    req = urllib.request.Request(args.url + path,
        data=None if data is None else json.dumps(data).encode(),
        headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=25) as response:
        return json.load(response)

def wait(predicate, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        state = api('/api/state')
        assert not state.get('fault'), state.get('fault')
        if predicate(state):
            return state
        time.sleep(.1)
    raise RuntimeError('referee timeout')

def rule(start, end, action, direction=None):
    r = {'from': start, 'to': end, 'action': action}
    if direction:
        r['direction'] = direction
    return r

template = api('/api/config/2')
results = []

def case(name, positions, actions, max_ticks, expected, event_name=None,
         event_start=1, event_params=None, resource=False, mission=None):
    folder = OUT / name
    folder.mkdir(exist_ok=True)
    cfg = copy.deepcopy(template)
    cfg['map']['obstacles'] = []
    cfg['map']['resources'] = ([{'id':'kept_resource','position':positions['A'],'value':10}] if resource else [])
    cfg['robots'] = [{'team': s, 'spawn': p} for s, p in positions.items()]
    cfg['missions'] = [] if mission is None else [mission]
    cfg['mission_text'] = '独立验收任务：' + json.dumps(cfg['missions'], ensure_ascii=False)
    cfg['events']['schedule'] = []
    cfg['events']['enabled'] = event_name is not None
    if event_name:
        cfg['events']['announcement_count'] = 1
        cfg['events']['announcement_delay_ticks'] = 1
        cfg['events']['definitions'][event_name].update(event_params or {})
        cfg['events']['schedule'] = [{'id':'fixture_event','event':event_name,'announce_tick':event_start-1}]
    (folder / 'practice_config.json').write_text(json.dumps(cfg, ensure_ascii=False, indent=2))
    api('/api/start', {'stage':2,'tick_ms':100,'max_ticks':max_ticks,'bots':[],'config':cfg})
    state = wait(lambda s:s['status'].get('ready'))
    plan = {'actions':{s:actions.get(s,[]) for s in positions},'max_ticks':max_ticks,'session_id':state['session_id']}
    planfile = folder/'probe_plan.json'
    planfile.write_text(json.dumps(plan, ensure_ascii=False, indent=2))
    env = os.environ.copy()
    env.update(ROS_DOMAIN_ID=args.domain, ROS_LOCALHOST_ONLY='0',
               RMW_IMPLEMENTATION='rmw_fastrtps_cpp', ROS_LOG_DIR=str(folder/'ros'))
    log = (folder/'probe_stdout.log').open('w')
    process = subprocess.Popen(['/bin/bash','-c','source /opt/ros/humble/setup.bash; exec "$@"',
        'probe',str(ROOT/'build/referee_probe'),str(planfile),str(folder/'frames.jsonl')],
        env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        time.sleep(2.5)
        assert process.poll() is None, 'probe failed before resume'
        api('/api/resume', {})
        state = wait(lambda s:s['status'].get('finished'),max_ticks*.1+15)
        process.wait(timeout=5)
        assert process.returncode==0, 'probe failed'
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGINT)
            process.wait(timeout=5)
        log.close()
    (folder/'final_state.json').write_text(json.dumps(state, ensure_ascii=False, indent=2))
    rows = [json.loads(line) for line in (folder/'frames.jsonl').read_text().splitlines()]
    latest = {}
    for row in rows:
        f = row['perception']
        assert f['session_id']==state['session_id']
        latest[row['seat']] = f
    for s in positions:
        frames = {x['perception']['tick'] for x in rows if x['seat']==s}
        assert frames==set(range(max_ticks+1)), (name,s,'missing ticks',set(range(max_ticks+1))-frames)
        assert latest[s]['tick']==max_ticks
        assert latest[s]['score']['invalid_actions']==0
    for s, fields in expected.items():
        f=latest[s]
        for key, expected_value in fields.items():
            actual = f['self'][key] if key in ('hp','alive','position') else f['score'][key]
            if isinstance(expected_value, (int,float)) and not isinstance(expected_value,bool):
                assert abs(actual-expected_value)<1e-8,(name,s,key,actual,expected_value)
            else:
                assert actual==expected_value,(name,s,key,actual,expected_value)
    # 练习 board 的 score 是含原始任务分的预览；不能据此认定正式 AI 资格。
    # 去掉原始任务项后，独立核对资源及500门槛的对抗项。
    for row in state['board']['teams']:
        expected_score = 30*min(row['resource_value']/120,1)+15*min(row['effective_damage']/500,1)+5*row['mission_completed']
        assert abs(row['score']-expected_score)<1e-8,(name,'500 score formula',row,expected_score)
    results.append({'case':name,'session_id':state['session_id'],'max_ticks':max_ticks,
                    'tick_ms':100,'expected':expected,'status':'pass'})
    (OUT/'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2))
    print('PASS',name,json.dumps({s:f['score']['effective_damage'] for s,f in latest.items()}),flush=True)

case('single_kill_then_death_retains_mission',{'A':[5,5],'B':[6,5]},
     {'A':[rule(0,5,'attack','right')],'B':[rule(10,15,'attack','left')]},16,
     {'A':{'effective_damage':100,'resource_value':10,'mission_completed_ids':['damage150']},
      'B':{'effective_damage':160}},resource=True,
     mission={'id':'damage150','type':'effective_damage','target':150})
case('mutual_kill_no_dead_damage_task',{'A':[5,5],'B':[6,5]},
     {'A':[rule(0,5,'attack','right')],'B':[rule(0,5,'attack','left')]},6,
     {'A':{'effective_damage':100,'mission_completed_ids':[]},
      'B':{'effective_damage':100,'mission_completed_ids':[]}},
     mission={'id':'damage100','type':'effective_damage','target':100})
case('shared_overkill',{'A':[5,5],'B':[6,5],'C':[7,5]},
     {'A':[rule(0,3,'attack','right')],'C':[rule(0,3,'attack','left')]},4,
     {'A':{'effective_damage':80},'C':{'effective_damage':80},'B':{'effective_damage':0}})
case('reflection_and_attack_share_one_reward',{'A':[7,5],'B':[6,5],'C':[5,5]},
     {'A':[rule(0,4,'defend')],'B':[rule(0,3,'attack','right')],'C':[rule(0,3,'attack','right')]},4,
     {'A':{'effective_damage':30},'C':{'effective_damage':90}},
     event_name='purity',event_params={'reflection_damage':20})
case('simultaneous_pepper_shared_rewards',{'A':[4,5],'B':[6,5],'C':[6,3],'D':[5,4]},
     {'A':[rule(1,2,'move','right')],'C':[rule(1,2,'move','down')]},3,
     {'A':{'effective_damage':60},'C':{'effective_damage':60},
      'B':{'effective_damage':0,'alive':False},'D':{'effective_damage':0,'alive':False}},
     event_name='pepper',event_params={'points':[[5,5],[6,4]],'damage':50})
case('combat_death_not_rewarded_by_later_fire',{'A':[5,4],'B':[6,5],'C':[5,5]},
     {'A':[rule(4,5,'move','right')],'C':[rule(0,5,'attack','right')]},6,
     {'A':{'effective_damage':0},'C':{'effective_damage':160}},
     event_name='pepper',event_params={'points':[[6,4]],'damage':50})
case('explosion_death_deducts_without_killer',{'A':[5,5],'B':[6,5]},
     {'A':[rule(0,5,'attack','right')]},8,
     {'A':{'effective_damage':100,'resource_value':10},'B':{'effective_damage':0}},
     event_name='explosion',event_start=6,resource=True,
     event_params={'damage':100,'rect':{'top_left':[5,5],'bottom_right':[5,5]}})
case('world_split_immediate_respawn_still_deducts',{'A':[5,5],'B':[6,5]},
     {'A':[rule(0,5,'attack','right'),rule(5,6,'move','down')]},8,
     {'A':{'effective_damage':100,'alive':True,'resource_value':10},'B':{'effective_damage':0}},
     event_name='world_split',event_start=6,resource=True,event_params={'point':[10,6]})
print('RESULT',len(results),'referee cases passed',flush=True)
