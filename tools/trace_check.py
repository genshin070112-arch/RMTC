#!/usr/bin/env python3
# 文件：tools/trace_check.py
# 作用：人工离线核对每场感知→动作→下一帧回执；不进入参赛进程。
# 不以最后一个动作Tick代替整场验证；恢复测试报告断流缺口，普通整场要求无缺口。
from practice_trace import combat_cancelled_move, forced_respawn_wait


def check_trace(records, session, ticks, strict=True):
    rows = {r['tick']: r for r in records
            if r.get('kind') == 'frame' and r.get('session_id') == session}
    missing = sorted(set(range(ticks + 1)) - rows.keys())
    missing_actions, missing_receipts, mismatches = [], [], []
    matched = 0
    cancelled_moves = []
    forced_waits = []
    for tick in range(ticks):
        row = rows.get(tick, {})
        action = row.get('action')
        if not action:
            missing_actions.append(tick)
            continue
        if action.get('tick') != tick or action.get('session_id') != session:
            mismatches.append({'tick': tick, 'reason': 'action identity'})
        next_row = rows.get(tick + 1, {})
        receipt = next_row.get('last_action')
        if not receipt or receipt.get('tick') != tick:
            missing_receipts.append(tick)
        elif forced_respawn_wait(row, next_row, receipt):
            forced_waits.append(tick)
        elif (receipt.get('action') != action.get('action') or
              (receipt.get('error') not in (None, 'E_BLOCKED', 'E_OOB')
               and not combat_cancelled_move(row, next_row, receipt))):
            mismatches.append({'tick': tick, 'reason': 'receipt rejected/different action', 'receipt': receipt})
        else:
            matched += 1
            if receipt.get('error') == 'E_STATE':
                cancelled_moves.append(tick)
    result = dict(session_id=session, matched_action_receipts=matched,
                  missing_frames=missing, missing_actions=missing_actions,
                  missing_receipts=missing_receipts, mismatches=mismatches,
                  combat_cancelled_move_ticks=cancelled_moves,
                  forced_respawn_wait_ticks=forced_waits)
    if mismatches or (strict and (missing or missing_actions or missing_receipts)):
        raise AssertionError(result)
    return result

if __name__ == '__main__':
    import copy
    records = [dict(kind='frame', session_id='s', tick=t,
                    action=dict(session_id='s', tick=t, action='move') if t < 3 else None,
                    last_action=dict(tick=t-1, action='move') if t else None) for t in range(4)]
    assert check_trace(records, 's', 3)['matched_action_receipts'] == 3
    for defect in ('frame', 'receipt', 'action'):
        bad = copy.deepcopy(records)
        if defect == 'frame': bad.pop(1)
        elif defect == 'receipt': bad[2]['last_action'] = None
        else: bad[2]['last_action']['action'] = 'wait'
        try: check_trace(bad, 's', 3)
        except AssertionError: print('PASS catches_missing_or_changed_' + defect)
        else: raise AssertionError('failed to catch ' + defect)
    assert check_trace(records[2:], 's', 3, strict=False)['missing_frames'] == [0, 1]
    print('PASS reports_recovery_gap')
