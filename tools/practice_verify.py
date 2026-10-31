#!/usr/bin/env python3
# 文件：tools/practice_verify.py
# 作用：人工练习验收工具（不参与参赛决策）
# 通过所供练习端的管理 API 创建测试、暂停/恢复、启动参赛程序并保存证据。
# 用法和 suite 选项见 README；输出 evidence/。只能用于独立练习验收，不向策略提供管理数据。
# 这份 Python 是离线测试驱动器；参赛逻辑、网关与 JSON 处理均在 src/ 的 C++ 中。

"""Offline human test driver ONLY. Contestant is C++; this is never a strategy input.
Uses the supplied practice controller's management API to create test cases and record outcomes.
"""
import argparse,json,os,random,signal,subprocess,time,urllib.request
from pathlib import Path
from trace_check import check_trace
from practice_trace import check_trace as check_recorded_trace, enrich_public_perceptions
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--suite',choices=['stage1','stage2','recovery','container','container_stage2'],required=True)
p.add_argument('--custom-stage2', action='store_true');p.add_argument('--url',default='http://127.0.0.1:18081');p.add_argument('--seat',default='A');p.add_argument('--domain',default='73');p.add_argument('--seeds',default='3,7,11,19')
p.add_argument('--evidence-prefix',default='v11_',help='独立存证前缀，保留旧版测试记录')
p.add_argument('--map-seed',type=int,default=20261023,help='离线自定义地图种子，不传给参赛策略')
p.add_argument('--contestant-binary',type=Path,help='离线对照试验指定旧C++可执行文件；需先加载ROS环境')
a=p.parse_args();out=R/'evidence'/(a.evidence_prefix+(a.suite+'_'+a.seat if a.suite=='stage2' else a.suite))
if out.exists() and any(out.iterdir()):
 p.error('证据目录已存在且非空，请使用新的 --evidence-prefix，避免混入旧场次或覆盖旧证据：'+str(out))
out.mkdir(parents=True,exist_ok=True)

def api(path,body=None):
 data=None if body is None else json.dumps(body).encode()
 req=urllib.request.Request(a.url+path,data=data,headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=25) as r:return json.load(r)

def wait(predicate,timeout=30,progress=False):
 deadline=time.monotonic()+timeout;last=-1
 while time.monotonic()<deadline:
  s=api('/api/state')
  if s.get('fault'):raise RuntimeError(s['fault'])
  tick=s['status'].get('tick',0)
  if progress and tick//50>last:
   last=tick//50;print('progress tick',tick,'scores',[(t['team'],t['score']) for t in s.get('board',{}).get('teams',[])],flush=True)
  if predicate(s):return s
  time.sleep(.08 if not progress else .3)
 raise RuntimeError('practice state timeout')

def deploy(stage,config=None,bots=None,max_ticks=None):
 settings={'stage':stage,'tick_ms':100 if stage==1 else 500,'max_ticks':max_ticks or (100 if stage==1 else 500),'bots':bots or []}
 if config is not None:settings['config']=config
 api('/api/start',settings);wait(lambda s:s['status'].get('ready'));time.sleep(2.5)

def randomized(seed):
 config=api('/api/config/1');rng=random.Random(seed);w=h=20;spawn=(1,1)
 free=[(x,y) for y in range(h) for x in range(w) if (x,y)!=spawn]
 rng.shuffle(free);walls=set(free[:40])
 reached={spawn};front=[spawn]
 for q in front:
  for dx,dy in [(1,0),(-1,0),(0,1),(0,-1)]:
   v=(q[0]+dx,q[1]+dy)
   if 0<=v[0]<w and 0<=v[1]<h and v not in walls and v not in reached:reached.add(v);front.append(v)
 positions=sorted(reached-{spawn});rng.shuffle(positions)
 config['map']={'width':w,'height':h,'obstacles':[list(q) for q in sorted(walls)],'resources':[
  {'id':f's{seed}_{i}','position':list(q),'value':10 if i<12 else 20} for i,q in enumerate(positions[:16])]}
 config['robots']=[{'team':'A','spawn':list(spawn)}]
 return config

env=os.environ.copy();env.update(ROS_DOMAIN_ID=a.domain,ROS_LOCALHOST_ONLY='0',RMW_IMPLEMENTATION='rmw_fastrtps_cpp',RMTC_TEAM_TOKEN='practice-'+a.seat)
proc=None;stream=None

def start(stage,index='main',container=False,max_ticks=None):
 global proc,stream
 logdir=out/('bot_'+index);logdir.mkdir(exist_ok=True)
 stream=(out/('stdout_'+index+'.log')).open('w')
 params=['--stage',str(stage),'--seat',a.seat,'--gateway',a.url,'--log-dir',str(logdir),'--decision-file',str(R/'config/decision_config.json')]
 if max_ticks:params+=['--max-ticks',str(max_ticks)]
 if container:
  params=['--stage',str(stage),'--seat',a.seat,'--gateway',a.url,'--log-dir','/app/logs','--decision-file','/app/config/decision_config.json']
  proc=subprocess.Popen(['docker','run','--rm','--name','rmtc-cpp-verification','--network','host','--ipc','host',
   '-e','ROS_DOMAIN_ID='+a.domain,'-e','RMTC_TEAM_TOKEN=practice-'+a.seat,'-v',str(logdir)+':/app/logs',
   'rmtc-cpp:1.1']+params,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
 else:
  proc=subprocess.Popen([str(a.contestant_binary.resolve()) if a.contestant_binary else str(R/'run.sh')]+params,env=env,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
 time.sleep(1.0)
 if proc.poll() is not None:raise RuntimeError('contestant failed; see stdout '+index)

def stop():
 global proc,stream
 if proc and proc.poll() is None:
  if a.suite in ('container','container_stage2'):subprocess.run(['docker','stop','-t','5','rmtc-cpp-verification'],capture_output=True,timeout=10)
  else:
   os.killpg(proc.pid,signal.SIGINT)
   try:proc.wait(timeout=8)
   except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=5)
 if stream:stream.close()
 proc=None;stream=None

results=[]
def save(label,s,stage,max_ticks):
 (out/(label+'.json')).write_text(json.dumps(s,ensure_ascii=False,indent=2))
 row=next(t for t in s['board']['teams'] if t['team']==a.seat)
 check=s['checks'][a.seat]
 assert s['status']['tick']==max_ticks,s['status']
 assert check.get('executed_tick')==max_ticks-1,check
 assert check['last_action']['tick']==max_ticks-1,check
 assert row['invalid_actions']==0,row
 if stage==1:assert row['resource_value']>0,'no resource collected'
 result={'case':label,'stage':stage,'max_ticks':max_ticks,'tick_ms':100 if stage==1 else 500,'seat':a.seat,
  'score':row['score'],'resource_value':row['resource_value'],'effective_damage':row.get('effective_damage',0),
  'missions':row.get('mission_completed',0),'invalid_actions':row['invalid_actions'],'executed_tick':check['executed_tick'],
  'session_id':s['session_id'],'run_dir':s['run_dir'],'ai':s.get('ai',{}).get(a.seat,{})}
 results.append(result);print('PASS',json.dumps(result,ensure_ascii=False),flush=True)

try:
 if a.suite=='stage1':
  start(1)
  cases=[('default_'+str(i),None) for i in range(3)]+[('random_'+seed,randomized(int(seed))) for seed in a.seeds.split(',') if seed]
  for label,config in cases:
   deploy(1,config);api('/api/resume',{});s=wait(lambda s:s['status'].get('finished'),18);time.sleep(.15);save(label,s,1,100)
 elif a.suite=='stage2':
  config=None
  if a.custom_stage2:
   config=api('/api/config/2');rng=random.Random(a.map_seed);spawns=[(1,1),(22,1),(1,22),(22,22)];target=(17,6)
   cells=[(x,y) for y in range(24) for x in range(24) if (x,y) not in spawns+[target]];rng.shuffle(cells)
   # 离线地图必须满足练习配置约束：事件拾取点不能落在静态障碍上。
   pickup={tuple(p) for definition in config.get('events',{}).get('definitions',{}).values() for p in definition.get('points',[])}
   walls=[q for q in cells[:55] if q not in pickup];positions=cells[55:71]
   config['map']={'width':24,'height':24,'obstacles':[list(q) for q in walls],
    'resources':[{'id':f'custom_{i}','position':list(q),'value':10 if i<12 else 20} for i,q in enumerate(positions)]}
   config['robots']=[{'team':seat,'spawn':list(pos)} for seat,pos in zip('ABCD',spawns)]
   config['missions']=[{'id':'damage_custom','type':'effective_damage','target':80.5},
    {'id':'collect_high_value','type':'high_resources','target':1},
    {'id':'reach_target','type':'reach','position':list(target)}]
   config['mission_text']='练习任务：调整后有效伤害达到80.5，一个高价值资源，到达 [17,6]。'
  start(2);deploy(2,config,bots=[s for s in 'ABCD' if s!=a.seat]);api('/api/resume',{})
  s=wait(lambda s:s['status'].get('finished'),275,True);time.sleep(.5);save('full_500_'+a.seat,s,2,500)
 elif a.suite=='recovery':
  start(1,'before');deploy(1);api('/api/resume',{});wait(lambda s:s['status'].get('tick',0)>=18)
  api('/api/pause',{});wait(lambda s:s['status'].get('paused'));time.sleep(.25);api('/api/resume',{})
  wait(lambda s:s['status'].get('tick',0)>=48);api('/api/pause',{});wait(lambda s:s['status'].get('paused'))
  stop();start(1,'after');api('/api/resume',{});s=wait(lambda s:s['status'].get('finished'),15);save('pause_restart',s,1,100)
  deploy(1);api('/api/resume',{});s=wait(lambda s:s['status'].get('finished'),18);save('same_process_new_session',s,1,100)
 elif a.suite=='container':
  start(1,container=True);deploy(1);api('/api/resume',{});s=wait(lambda s:s['status'].get('finished'),20);save('container_full_stage1',s,1,100)
 elif a.suite=='container_stage2':
  start(2,container=True);deploy(2,bots=[s for s in 'ABCD' if s!=a.seat]);api('/api/resume',{})
  s=wait(lambda s:s['status'].get('finished'),275,True);time.sleep(.5);save('container_full_500_'+a.seat,s,2,500)
finally:stop()
(out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
frames=[];errors=[];llm=[]
for f in out.glob('bot_*/*.jsonl'):
 for line in f.read_text().splitlines():
  record=json.loads(line)
  if record.get('kind')=='frame':frames.append(record)
  if record.get('kind')=='error':errors.append(record)
  if record.get('kind')=='llm_result':llm.append(record)
assert not errors,errors[:3]
assert frames,'no contestant frame logs'
assert all(r['action'] is None or (r['action']['session_id']==r['session_id'] and r['action']['tick']==r['tick']) for r in frames)
latency=sorted(r['processing_us'] for r in frames)
summary={'cases':results,'frame_count':len(frames),'latency_p95_us':latency[int(.95*(len(latency)-1))],
 'latency_max_us':max(latency),'errors':errors,'llm_responses':len(llm),'model_schema_invalid':sum(r.get('error')=='model_schema_invalid' for r in llm),
 'same_tick_replays':sum(r.get('mode')=='replay' for r in frames),'ai_mode':'practice mock; never formal AI qualification'}
public_evidence=enrich_public_perceptions(frames,results)
(out/'public_respawn_evidence.json').write_text(json.dumps(public_evidence,ensure_ascii=False,indent=2))
summary['per_session_trace']=[check_trace(frames,r['session_id'],r['max_ticks'],strict=a.suite!='recovery') for r in results]
traces=[check_recorded_trace(frames,case['session_id'],case['max_ticks'],require_complete=a.suite!='recovery') for case in results]
summary['execution_traces']=traces
summary['ignored_other_sessions']=sorted({r['session_id'] for r in frames}-{case['session_id'] for case in results})
summary['trace_check_passed']=all(t['passed'] for t in traces)
(out/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2))
assert summary['trace_check_passed'],[t for t in traces if not t['passed']]
print('SUMMARY',json.dumps({k:v for k,v in summary.items() if k not in ('cases','errors')},ensure_ascii=False),flush=True)
