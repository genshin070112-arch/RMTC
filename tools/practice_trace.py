#!/usr/bin/env python3
# 文件：tools/practice_trace.py
# 作用：人工验收专用的逐 Tick 感知、发布和执行回执核对；不进入参赛程序。
# 正常整场要求完整覆盖，恢复测试保留并报告实际缺口，不把丢帧伪装成完整联调。


def combat_cancelled_move(previous, current, receipt):
    """v1.1：合法MOVE在本Tick战斗中被击毁后取消，不增加无效动作。"""
    before_count = previous.get('score', {}).get('invalid_actions')
    after_count = current.get('score', {}).get('invalid_actions')
    return (receipt.get('error') == 'E_STATE'
            and receipt.get('action') == 'move' and receipt.get('moved') is False
            and (previous.get('action') or {}).get('action') == 'move'
            and previous.get('hp', 0) > 0 and current.get('hp', -1) == 0
            and before_count is not None and before_count == after_count
            and receipt.get('tick') == previous.get('tick')
            and current.get('tick') == previous.get('tick', -2) + 1)


def forced_respawn_wait(previous, current, receipt):
    """v1.1 §5.2：事件阶段立即重生者强制WAIT，不冒称原动作已执行。"""
    before = previous.get('score', {})
    after = current.get('score', {})
    damage = before.get('effective_damage')
    return (receipt.get('action') == 'wait' and receipt.get('moved') is False
            and receipt.get('error') is None
            and (previous.get('action') or {}).get('action') in ('move','attack','defend')
            and current.get('position_reason') == 'respawn'
            and current.get('alive') is True and current.get('protected') is True
            and previous.get('hp',0) > 0 and current.get('hp') == 100
            and isinstance(damage,(int,float))
            and after.get('effective_damage') == max(0,damage-60)
            and before.get('invalid_actions') is not None
            and before.get('invalid_actions') == after.get('invalid_actions')
            and receipt.get('tick') == previous.get('tick')
            and current.get('tick') == previous.get('tick',-2)+1)


def enrich_public_perceptions(rows, cases):
    """人工离线验收补充公开self字段；不读取隐藏状态，不更改原始机器人日志。

    原审计帧未包含position_reason，不能凭位置跳变猜重生；仅从本席位同场
    同Tick的练习通信记录补充，且先核对位置/HP/回执完全一致。返回证据供归档。
    """
    import json
    from pathlib import Path
    evidence=[]
    for case in cases:
        selected={r['tick']:r for r in rows if r.get('kind')=='frame'
                  and r.get('session_id')==case['session_id']}
        path=Path(case['run_dir'])/'communication.jsonl'
        if not path.exists():
            continue  # 缺少证据时不放宽动作错配检查。
        with path.open() as stream:
            for line in stream:
                packet=json.loads(line)
                if packet.get('kind')!='perception' or packet.get('seat')!=case['seat']:
                    continue
                data=packet.get('data',{})
                if data.get('session_id')!=case['session_id']:
                    continue
                row=selected.get(data.get('tick'))
                if row is None:
                    continue
                own=data['self']
                if (row.get('position')!=own.get('position') or row.get('hp')!=own.get('hp')
                        or row.get('last_action')!=own.get('last_action')):
                    raise AssertionError('public perception does not match saved contestant frame')
                for key in ('position_reason','alive','protected'):
                    if key in own:row[key]=own[key]
                if own.get('position_reason')=='respawn':
                    evidence.append({'source':str(path),'seat':case['seat'],'data':data})
    return evidence


def check_trace(rows, session_id, max_ticks, require_complete=True):
    frames = [r for r in rows if r.get('kind') == 'frame'
              and r.get('session_id') == session_id]
    sent, receipts, duplicate_changes, receipt_errors = {}, {}, [], []
    action_identity_errors = []
    by_tick = {r['tick']: r for r in frames}
    cancelled_moves = []
    forced_waits = []
    for row in frames:
        action = row.get('action')
        if action is not None:
            tick = row['tick']
            if action.get('tick') != tick or action.get('session_id') != session_id:
                action_identity_errors.append(tick)
            if tick in sent and sent[tick] != action:
                duplicate_changes.append(tick)
            sent[tick] = action
        receipt = row.get('last_action')
        if isinstance(receipt, dict) and isinstance(receipt.get('tick'), int):
            tick = receipt['tick']
            cancelled = combat_cancelled_move(by_tick.get(tick, {}), row, receipt)
            if cancelled:
                cancelled_moves.append(tick)
            if forced_respawn_wait(by_tick.get(tick, {}), row, receipt):
                forced_waits.append(tick)
            if ((receipt.get('error') not in (None, 'E_BLOCKED', 'E_OOB') and not cancelled)
                    or tick >= row['tick']):
                receipt_errors.append({'frame_tick': row['tick'], 'receipt': receipt})
            receipts[tick] = receipt
    mismatches = [t for t in sorted(sent.keys() & receipts.keys())
                  if sent[t].get('action') != receipts[t].get('action') and t not in forced_waits]
    frame_ticks = {r['tick'] for r in frames}
    expected_actions = set(range(max_ticks))
    report = {
        'session_id': session_id,
        'complete_coverage_required': require_complete,
        'frame_count': len(frames),
        'distinct_frame_ticks': len(frame_ticks),
        'sent_action_ticks': len(sent),
        'received_execution_ticks': len(receipts),
        'missing_frame_ticks': sorted(set(range(max_ticks + 1)) - frame_ticks),
        'missing_action_ticks': sorted(expected_actions - sent.keys()),
        'missing_receipt_ticks': sorted(expected_actions - receipts.keys()),
        'sent_without_receipt_ticks': sorted(sent.keys() - receipts.keys()),
        'unexpected_frame_ticks': sorted(frame_ticks - set(range(max_ticks + 1))),
        'unexpected_action_ticks': sorted(sent.keys() - expected_actions),
        'changed_duplicate_action_ticks': sorted(set(duplicate_changes)),
        'action_identity_error_ticks': action_identity_errors,
        'action_type_mismatch_ticks': mismatches,
        'receipt_errors': receipt_errors,
        'combat_cancelled_move_ticks': sorted(set(cancelled_moves)),
        'forced_respawn_wait_ticks': sorted(set(forced_waits)),
    }
    errors = [key for key in ('unexpected_frame_ticks', 'unexpected_action_ticks',
              'changed_duplicate_action_ticks', 'action_identity_error_ticks',
              'action_type_mismatch_ticks', 'receipt_errors') if report[key]]
    if not frames:
        errors.append('no_frames_for_session')
    if require_complete:
        errors += [key for key in ('missing_frame_ticks', 'missing_action_ticks',
                   'missing_receipt_ticks', 'sent_without_receipt_ticks') if report[key]]
    report['errors'] = errors
    report['passed'] = not errors
    return report
