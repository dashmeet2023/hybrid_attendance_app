"""NFC/RFID cards tapped on a classroom reader (ESP32 + RC522/PN532 attached to the room's Pi).

The reader host authenticates with the same per-room token as the CSI sensor, so a tap
proves the card was physically at that room's reader. A card is linked to one student by
the admin; tapping it marks that student present in the open session for that room.

Card UIDs are not secret (any phone can read them) and a card can be lent, so the CSI
room-presence check still applies when the teacher requires it, and every tap is logged.
"""
import json
import re
import time

from sqlalchemy import select, insert, update, and_

from .database import (nfc_cards, csi_sensors, class_sessions, courses, session_roster,
                       attendance, devices, users)
from . import verification as verify

UID_RE = re.compile(r'^[0-9A-F]{8,20}$')


def normalize_uid(value):
    uid = re.sub(r'[^0-9A-Fa-f]', '', value if isinstance(value, str) else '').upper()
    if not UID_RE.match(uid):
        from .service import ApiError
        raise ApiError('uid must be 4-10 bytes of hex', 422)
    return uid


def masked(uid):
    return '…' + uid[-4:]


STAFF_SESSION_MINUTES = 60


def tap(service, token, data, now=None, ip=''):
    """Handle a card tap from a room reader.

    student card -> mark present in the open session for this room (CSI rules apply when required)
    teacher card -> open a 60-minute session for the teacher's class in this room, or close the open one
    admin card   -> ADMIN (the Pi opens the live website for administration)
    """
    from .service import ApiError, digest, uid as new_id, log
    engine = service.engine
    now = time.time() if now is None else now
    if not token:
        raise ApiError('Reader token required', 401)
    card_uid = normalize_uid(data.get('uid'))
    checks, student_id, session_id = [], None, None
    with engine.begin() as conn:
        reader = conn.execute(select(csi_sensors).where(csi_sensors.c.token_hash == digest(token),
            csi_sensors.c.active == 1)).mappings().first()
        if not reader:
            raise ApiError('Reader token is invalid or revoked', 401)
        room = reader['room']
        card = conn.execute(select(nfc_cards).where(nfc_cards.c.uid == card_uid)).mappings().first()
        if not card:
            conn.execute(insert(nfc_cards).values(id=new_id(), uid=card_uid, active=1, created_at=now,
                last_seen_at=now, last_room=room))
            return {'outcome': 'UNKNOWN_CARD', 'uid': card_uid,
                    'message': 'New card saved. Assign it to a person in Administration > NFC cards.'}
        conn.execute(update(nfc_cards).where(nfc_cards.c.id == card['id']).values(last_seen_at=now, last_room=room))
        if not card['student_id']:
            return {'outcome': 'UNKNOWN_CARD', 'uid': card_uid,
                    'message': 'Card is not assigned yet. Assign it in Administration > NFC cards.'}
        if not card['active']:
            return {'outcome': 'REJECTED', 'uid': card_uid, 'message': 'This card has been revoked'}
        person = conn.execute(select(users).where(users.c.id == card['student_id'])).mappings().first()
        person = dict(person)
        if person['role'] == 'admin':
            log(conn, person['id'], 'admin_card_tap', room)
            return {'outcome': 'ADMIN', 'uid': card_uid, 'student': person['name'], 'name': person['name'],
                    'role': 'admin', 'message': 'Administrator: opening the live website'}
        if person['role'] == 'teacher':
            teacher = person
        else:
            teacher = None
            student = person
    if teacher is not None:
        return teacher_tap(service, teacher, room, card_uid, now, ip)

    with engine.begin() as conn:
        student_id = student['id']
        checks.append(verify.check('identity', 'pass', f"NFC card {masked(card_uid)} of {student['name']}"))
        checks.append(verify.check('reader', 'pass', f'Tapped on the reader in {room}'))
        # Open sessions in this room for which the student is on the roster.
        session = conn.execute(select(class_sessions, courses.c.code, courses.c.room).join(
            courses, courses.c.id == class_sessions.c.course_id).join(session_roster, and_(
                session_roster.c.session_id == class_sessions.c.id, session_roster.c.student_id == student_id)).where(
            courses.c.room == room, class_sessions.c.status.in_(['open', 'scheduled']),
            class_sessions.c.starts_at <= now, class_sessions.c.ends_at > now).order_by(
            class_sessions.c.starts_at.desc()).limit(1)).mappings().first()
        if not session:
            checks.append(verify.check('session', 'fail', f'No open session in {room} for this student'))
            outcome, reason, reading = 'REJECTED', checks[-1]['detail'], None
        else:
            session_id = session['id']
            # Serialise with other check-ins and with closing the session (row lock).
            conn.execute(update(class_sessions).where(class_sessions.c.id == session_id).values(
                status=class_sessions.c.status))
            checks.append(verify.check('session', 'pass', f"{session['code']} session is active"))
            existing = conn.execute(select(attendance.c.recorded_at).where(attendance.c.session_id == session_id,
                attendance.c.student_id == student_id)).first()
            if existing:
                return {'outcome': 'VERIFIED', 'duplicate': True, 'student': student['name'], 'uid': card_uid,
                        'message': f"{student['name']} was already marked present", 'checks': checks}
            policy = verify.policy_for(conn, session_id)
            result, reading = verify.csi_check(conn, policy, session, room, now)
            checks.append(result)
            outcome = verify.outcome_of(checks)
            reason = next((c['detail'] for c in checks if c['status'] in ('fail', 'pending')), '')
            if outcome == 'VERIFIED':
                conn.execute(insert(attendance).values(id=new_id(), session_id=session_id, student_id=student_id,
                    device_id=card['device_id'], recorded_at=now, distance_m=None, network_checked=0))
                log(conn, student_id, 'attendance_recorded_nfc', session_id)
    verify.record_check(engine, session_id, student_id, outcome, reason or 'NFC card tap', checks, reading)
    message = {'VERIFIED': f"{student['name']} marked present", 'PENDING': reason, 'REJECTED': reason}[outcome]
    return {'outcome': outcome, 'student': student['name'], 'uid': card_uid, 'message': message, 'checks': checks}


def teacher_tap(service, teacher, room, card_uid, now, ip):
    """Toggle the attendance session for the teacher's class in this room (same rules as the website)."""
    from .service import ApiError
    base = {'uid': card_uid, 'student': teacher['name'], 'name': teacher['name'], 'role': 'teacher'}
    with service.engine.connect() as conn:
        mine = conn.execute(select(courses.c.id, courses.c.code).where(courses.c.teacher_id == teacher['id'],
            courses.c.room == room, courses.c.archived == 0).order_by(courses.c.code)).mappings().all()
        if not mine:
            return {**base, 'outcome': 'REJECTED', 'message': f"{teacher['name']} has no class in {room}"}
        ids = [c['id'] for c in mine]
        active = conn.execute(select(class_sessions.c.id, courses.c.code).join(courses, courses.c.id == class_sessions.c.course_id).where(
            class_sessions.c.course_id.in_(ids), class_sessions.c.status.in_(['open', 'scheduled']),
            class_sessions.c.starts_at <= now, class_sessions.c.ends_at > now)).mappings().first()
        present = 0
        if active:
            present = conn.execute(select(attendance.c.id).where(attendance.c.session_id == active['id'])).all()
            present = len(present)
    try:
        if active:
            service.mutate(teacher, 'sessions/close', {'session_id': active['id']}, None, ip)
            return {**base, 'outcome': 'SESSION_CLOSED',
                    'message': f"{active['code']} session closed: {present} present"}
        if len(mine) > 1:
            return {**base, 'outcome': 'REJECTED',
                    'message': f"Several of your classes use {room}; open the session on the website"}
        service.mutate(teacher, 'sessions/open', {'course_id': mine[0]['id'], 'minutes': STAFF_SESSION_MINUTES}, None, ip)
        return {**base, 'outcome': 'SESSION_OPENED',
                'message': f"{mine[0]['code']} session opened for {STAFF_SESSION_MINUTES} min"}
    except ApiError as error:
        return {**base, 'outcome': 'REJECTED', 'message': error.message}


def assign(conn, admin, data, now=None):
    """Link a card (seen by a reader, or typed in by UID) to a student; one active card per student."""
    from .service import ApiError, digest, uid as new_id, log, field
    now = time.time() if now is None else now
    student_id = field(data, 'student_id', 40)   # the person the card belongs to (student, teacher or admin)
    student = conn.execute(select(users).where(users.c.id == student_id)).mappings().first()
    if not student or student['role'] not in ('student', 'teacher', 'admin'):
        raise ApiError('Choose an account')
    if data.get('card_id'):
        card = conn.execute(select(nfc_cards).where(nfc_cards.c.id == field(data, 'card_id', 40))).mappings().first()
        if not card:
            raise ApiError('Card not found', 404)
    else:
        card_uid = normalize_uid(data.get('uid'))
        card = conn.execute(select(nfc_cards).where(nfc_cards.c.uid == card_uid)).mappings().first()
        if not card:
            card_id = new_id()
            conn.execute(insert(nfc_cards).values(id=card_id, uid=card_uid, active=1, created_at=now))
            card = conn.execute(select(nfc_cards).where(nfc_cards.c.id == card_id)).mappings().first()
    if student['role'] != 'student':
        # Staff cards open/close sessions (teacher) or open administration (admin); no attendance device needed.
        conn.execute(update(nfc_cards).where(nfc_cards.c.student_id == student_id, nfc_cards.c.id != card['id']).values(active=0))
        conn.execute(update(nfc_cards).where(nfc_cards.c.id == card['id']).values(student_id=student_id, device_id=None, active=1))
        log(conn, admin['id'], 'nfc_card_assigned', card['id'])
        return {'message': f"Card {masked(card['uid'])} assigned to {student['name']} ({student['role']})"}
    # Attendance rows reference a device; a card gets its own device record (status 'card',
    # so approving or revoking phones never touches it and it stays out of the phone list).
    token_hash = digest('nfc:' + card['uid'])
    device = conn.execute(select(devices.c.id).where(devices.c.user_id == student_id,
        devices.c.token_hash == token_hash)).first()
    if device:
        device_id = device.id
        conn.execute(update(devices).where(devices.c.id == device_id).values(status='card'))
    else:
        device_id = new_id()
        conn.execute(insert(devices).values(id=device_id, user_id=student_id, token_hash=token_hash,
            label=f"NFC card {masked(card['uid'])}", status='card', created_at=now))
    conn.execute(update(nfc_cards).where(nfc_cards.c.student_id == student_id, nfc_cards.c.id != card['id']).values(active=0))
    conn.execute(update(nfc_cards).where(nfc_cards.c.id == card['id']).values(student_id=student_id,
        device_id=device_id, active=1))
    log(conn, admin['id'], 'nfc_card_assigned', card['id'])
    return {'message': f"Card {masked(card['uid'])} assigned to {student['name']}"}


def revoke(conn, admin, data):
    from .service import ApiError, log, field
    card_id = field(data, 'card_id', 40)
    if conn.execute(update(nfc_cards).where(nfc_cards.c.id == card_id).values(active=0)).rowcount != 1:
        raise ApiError('Card not found', 404)
    log(conn, admin['id'], 'nfc_card_revoked', card_id)
    return {'message': 'Card revoked'}


def card_list(conn):
    rows = conn.execute(select(nfc_cards.c.id, nfc_cards.c.uid, nfc_cards.c.student_id, nfc_cards.c.active,
        nfc_cards.c.created_at, nfc_cards.c.last_seen_at, nfc_cards.c.last_room, users.c.name, users.c.email, users.c.role).outerjoin(
        users, users.c.id == nfc_cards.c.student_id).order_by(nfc_cards.c.created_at.desc())).mappings().all()
    return [dict(r) for r in rows]
