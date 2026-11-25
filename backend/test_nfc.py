"""NFC card check-in: unknown card, assignment, tap, duplicates, CSI gating, revocation."""
import json
import urllib.error
import urllib.request

from backend import test_verification


class NfcTests(test_verification.VerificationTests):
    # Reuse the fixture only; do not re-run the verification suite under this class.
    for _name in [n for n in dir(test_verification.VerificationTests) if n.startswith('test_')]:
        locals()[_name] = None

    def tap(self, token, uid):
        request = urllib.request.Request(self.url + '/api/nfc/tap', data=json.dumps({'uid': uid}).encode(),
            headers={'Content-Type': 'application/json', **({'Authorization': 'Bearer ' + token} if token else {})})
        try:
            response = urllib.request.urlopen(request, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, json.loads(response.read())

    def card(self, uid):
        return next(c for c in self.state(self.admin)['nfc_cards'] if c['uid'] == uid)

    def test_card_tap_marks_present_once(self):
        token = self.sensor()
        self.assertEqual(self.tap(None, '04A1B2C3')[0], 401)
        self.assertEqual(self.tap('forged', '04A1B2C3')[0], 401)
        self.assertEqual(self.tap(token, 'zz')[0], 422)
        # First tap of a new card stores it unassigned.
        status, result = self.tap(token, '04:a1:b2:c3')
        self.assertEqual((status, result['outcome']), (200, 'UNKNOWN_CARD'))
        card = self.card('04A1B2C3')
        self.assertIsNone(card['student_id'])
        self.assertEqual(card['last_room'], 'Lab 3B')
        # Only admins assign; students cannot.
        self.assertEqual(self.student.request('nfc/cards/assign', dict(card_id=card['id'], student_id=self.student_id))[0], 403)
        status, result = self.admin.request('nfc/cards/assign', dict(card_id=card['id'], student_id=self.student_id))
        self.assertEqual(status, 200, result)
        # No session open yet.
        status, result = self.tap(token, '04A1B2C3')
        self.assertEqual(result['outcome'], 'REJECTED')
        sid, _ = self.open_session()
        status, result = self.tap(token, '04A1B2C3')
        self.assertEqual((status, result['outcome']), (200, 'VERIFIED'), result)
        self.assertEqual(result['student'], 'Student One')
        status, again = self.tap(token, '04A1B2C3')
        self.assertTrue(again['duplicate'])
        report = self.state(self.teacher)['reports']
        self.assertEqual([r['attendance_status'] for r in report if r['session_id'] == sid].count('Present'), 1)
        # Card device records stay out of the phone list; attempts are visible to the teacher.
        self.assertFalse(any(d['status'] == 'card' for d in self.state(self.admin)['devices']))
        self.assertTrue(any(c['outcome'] == 'VERIFIED' for c in self.state(self.teacher)['checks']))

    def test_reader_in_another_room_and_csi_gate(self):
        other = self.sensor('Room 204')
        token = self.sensor()
        status, _ = self.admin.request('nfc/cards/assign', dict(uid='04A1B2C3', student_id=self.student_id))
        self.assertEqual(status, 200)
        self.open_session(code_required=False, csi_required=True)
        self.assertEqual(self.tap(other, '04A1B2C3')[1]['outcome'], 'REJECTED')
        self.assertEqual(self.tap(token, '04A1B2C3')[1]['outcome'], 'PENDING')  # no CSI reading yet
        self.post_csi(token, 'ABSENT', confidence=0.9)
        self.assertEqual(self.tap(token, '04A1B2C3')[1]['outcome'], 'REJECTED')
        self.post_csi(token, 'PRESENT', confidence=0.9)
        self.assertEqual(self.tap(token, '04A1B2C3')[1]['outcome'], 'VERIFIED')

    def test_revoke_and_reassign(self):
        token = self.sensor()
        self.admin.request('nfc/cards/assign', dict(uid='04A1B2C3', student_id=self.student_id))
        card = self.card('04A1B2C3')
        self.open_session()
        self.assertEqual(self.admin.request('nfc/cards/revoke', dict(card_id=card['id']))[0], 200)
        self.assertEqual(self.tap(token, '04A1B2C3')[1]['outcome'], 'REJECTED')
        # A new card for the same student deactivates the old one.
        self.admin.request('nfc/cards/assign', dict(card_id=card['id'], student_id=self.student_id))
        self.admin.request('nfc/cards/assign', dict(uid='04FFEEDD', student_id=self.student_id))
        self.assertEqual(self.card('04A1B2C3')['active'], 0)
        self.assertEqual(self.tap(token, '04FFEEDD')[1]['outcome'], 'VERIFIED')
        # A phone approval does not revoke the card.
        self.approve_phone(self.student, test_verification.PHONE_A)
        self.assertEqual(self.card('04FFEEDD')['active'], 1)



class StaffCardTests(test_verification.VerificationTests):
    for _name in [n for n in dir(test_verification.VerificationTests) if n.startswith('test_')]:
        locals()[_name] = None
    tap = NfcTests.tap
    card = NfcTests.card

    def user_id(self, role):
        return next(u['id'] for u in self.state(self.admin)['users'] if u['role'] == role)

    def test_teacher_card_opens_and_closes_session(self):
        token = self.sensor()
        teacher_id = self.user_id('teacher')
        self.assertEqual(self.admin.request('nfc/cards/assign', dict(uid='0A0B0C0D', student_id=teacher_id))[0], 200)
        self.admin.request('nfc/cards/assign', dict(uid='04A1B2C3', student_id=self.student_id))
        # Reader in another room: the teacher has no class there.
        other = self.sensor('Room 204')
        self.assertEqual(self.tap(other, '0A0B0C0D')[1]['outcome'], 'REJECTED')
        status, result = self.tap(token, '0A0B0C0D')
        self.assertEqual((status, result['outcome']), (200, 'SESSION_OPENED'), result)
        sessions = [s for s in self.state(self.teacher)['sessions'] if s['status'] == 'open']
        self.assertEqual(len(sessions), 1)
        self.assertEqual(self.tap(token, '04A1B2C3')[1]['outcome'], 'VERIFIED')
        status, result = self.tap(token, '0A0B0C0D')
        self.assertEqual(result['outcome'], 'SESSION_CLOSED', result)
        self.assertIn('1 present', result['message'])
        self.assertFalse([s for s in self.state(self.teacher)['sessions'] if s['status'] == 'open'])
        # Staff cards do not create attendance devices.
        self.assertEqual(self.card('0A0B0C0D')['role'], 'teacher')

    def test_admin_card(self):
        token = self.sensor()
        admin_id = self.user_id('admin')
        self.admin.request('nfc/cards/assign', dict(uid='0B0B0B0B', student_id=admin_id))
        status, result = self.tap(token, '0B0B0B0B')
        self.assertEqual((status, result['outcome']), (200, 'ADMIN'), result)
