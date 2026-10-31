#!/usr/bin/env python3
# 文件：tools/test_practice_trace.py
# 作用：离线验证人工验收器确实能发现漏帧、漏回执、动作不一致和重复帧改变。
# 使用内存夹具，不运行机器人或模型，不作为参赛策略。运行：python3 tools/test_practice_trace.py。
import copy
import unittest
from practice_trace import check_trace
from trace_check import check_trace as check_next_frame


def fixture():
    return [{'kind': 'frame', 'session_id': 'current', 'tick': tick,
             'action': {'session_id': 'current', 'tick': tick, 'action': 'wait'}
                       if tick < 3 else None,
             'last_action': {'tick': tick - 1, 'action': 'wait'} if tick else None}
            for tick in range(4)]


class TraceChecks(unittest.TestCase):
    def state_cancel_fixture(self):
        rows = fixture()
        for row in rows:
            row['hp'] = 100
            row['score'] = {'invalid_actions': 0}
        rows[0]['hp'] = 10
        rows[0]['action']['action'] = 'move'
        rows[1]['hp'] = 0
        rows[1]['last_action'] = {'tick': 0, 'action': 'move',
                                 'moved': False, 'error': 'E_STATE'}
        return rows

    def test_legal_move_cancelled_by_combat_death(self):
        rows = self.state_cancel_fixture()
        self.assertTrue(check_trace(rows, 'current', 3)['passed'])
        self.assertEqual(check_next_frame(rows, 'current', 3)['matched_action_receipts'], 3)

    def test_state_error_requires_death_evidence(self):
        rows = self.state_cancel_fixture()
        rows[1]['hp'] = 100
        self.assertFalse(check_trace(rows, 'current', 3)['passed'])
        with self.assertRaises(AssertionError):
            check_next_frame(rows, 'current', 3)

    def test_state_error_with_invalid_count_increase_rejected(self):
        rows = self.state_cancel_fixture()
        rows[1]['score']['invalid_actions'] = 1
        self.assertFalse(check_trace(rows, 'current', 3)['passed'])
        with self.assertRaises(AssertionError):
            check_next_frame(rows, 'current', 3)

    def test_dead_move_submission_rejected(self):
        rows = self.state_cancel_fixture()
        rows[0]['hp'] = 0
        self.assertFalse(check_trace(rows, 'current', 3)['passed'])
        with self.assertRaises(AssertionError):
            check_next_frame(rows, 'current', 3)

    def split_fixture(self):
        rows = fixture()
        for row in rows:
            row.update(hp=100, position=[1,1], alive=True, protected=False,
                       score={'invalid_actions':0,'effective_damage':420})
        rows[0].update(position=[4,8], score={'invalid_actions':0,'effective_damage':480})
        rows[0]['action']['action']='move'
        rows[1].update(position_reason='respawn',protected=True)
        rows[1]['last_action'].update(moved=False)
        return rows

    def test_forced_respawn_wait_is_separate_rule_override(self):
        rows=self.split_fixture()
        result=check_trace(rows,'current',3)
        self.assertTrue(result['passed'])
        self.assertEqual(result['forced_respawn_wait_ticks'],[0])
        result=check_next_frame(rows,'current',3)
        self.assertEqual(result['forced_respawn_wait_ticks'],[0])
        self.assertEqual(result['matched_action_receipts'],2)

    def test_wait_override_requires_explicit_respawn_evidence(self):
        rows=self.split_fixture(); del rows[1]['position_reason']
        self.assertFalse(check_trace(rows,'current',3)['passed'])
        with self.assertRaises(AssertionError):check_next_frame(rows,'current',3)

    def test_wait_override_requires_death_deduction_and_no_invalid(self):
        for field,value in [('effective_damage',480),('invalid_actions',1)]:
            rows=self.split_fixture(); rows[1]['score'][field]=value
            self.assertFalse(check_trace(rows,'current',3)['passed'])
            with self.assertRaises(AssertionError):check_next_frame(rows,'current',3)

    def test_ordinary_respawn_not_misread_as_same_tick_override(self):
        rows=self.split_fixture();rows[0]['hp']=0
        self.assertFalse(check_trace(rows,'current',3)['passed'])
        with self.assertRaises(AssertionError):check_next_frame(rows,'current',3)

    def test_complete_match(self):
        self.assertTrue(check_trace(fixture(), 'current', 3)['passed'])

    def test_missing_middle_frame(self):
        rows = fixture()
        del rows[1]
        result = check_trace(rows, 'current', 3)
        self.assertFalse(result['passed'])
        self.assertEqual(result['missing_frame_ticks'], [1])

    def test_missing_receipt(self):
        rows = fixture()
        rows[1]['last_action'] = None
        self.assertEqual(check_trace(rows, 'current', 3)['missing_receipt_ticks'], [0])
        self.assertFalse(check_trace(rows, 'current', 3)['passed'])

    def test_wrong_executed_action(self):
        rows = fixture()
        rows[1]['last_action']['action'] = 'move'
        self.assertEqual(check_trace(rows, 'current', 3)['action_type_mismatch_ticks'], [0])
        self.assertFalse(check_trace(rows, 'current', 3)['passed'])

    def test_recovery_reports_gap(self):
        rows = fixture()
        del rows[1]
        result = check_trace(rows, 'current', 3, require_complete=False)
        self.assertTrue(result['passed'])
        self.assertEqual(result['missing_frame_ticks'], [1])

    def test_foreign_session_does_not_fill_gap(self):
        rows = fixture()
        rows[1]['session_id'] = 'old'
        self.assertFalse(check_trace(rows, 'current', 3)['passed'])

    def test_duplicate_is_cached(self):
        rows = fixture()
        rows.insert(1, copy.deepcopy(rows[0]))
        self.assertTrue(check_trace(rows, 'current', 3)['passed'])
        rows[1]['action']['action'] = 'defend'
        self.assertEqual(check_trace(rows, 'current', 3)['changed_duplicate_action_ticks'], [0])

    def test_invalid_receipt(self):
        rows = fixture()
        rows[1]['last_action']['error'] = 'invalid_action'
        self.assertFalse(check_trace(rows, 'current', 3)['passed'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
