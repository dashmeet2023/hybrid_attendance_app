#!/usr/bin/env python3
"""
One-shot classroom card setup through the attendance website's own API (same rules as the website):
  1. creates any missing student accounts,
  2. enrolls the students in the class (as the teacher),
  3. assigns every card UID to its person (as the admin).

Edit CLASS_CODE / STUDENTS / CARDS below, then on the Pi:
  ~/hybrid_attendance_app/csi/venv/bin/python ~/hybrid_attendance_app/nfc/setup_cards.py
Passwords are typed in when it runs (hidden) and are never stored. Safe to run again.
The server address comes from BACKEND_URL in csi/.env.
"""
import getpass
import os
import sys
from pathlib import Path

import requests

CLASS_CODE = "CS101"
ADMIN_EMAIL = "xyz@gmail.com"
TEACHER_EMAIL = "123@gmail.com"

# Students: email -> (name, roll number). Missing accounts are created.
STUDENTS = {
    "student1@gmail.com": ("student1", "CS01"),
    "student2@gmail.com": ("student2", "CS02"),
    "student3@gmail.com": ("student3", "CS03"),
}

# Card UID -> email of the person who owns it (student, teacher or admin).
CARDS = {
    "93E10B42": "student1@gmail.com",
    "1149F703": "student2@gmail.com",
    "71781218": "student3@gmail.com",
    "E3E7CAF5": TEACHER_EMAIL,   # teacher card: opens/closes the session
    "D9636C06": ADMIN_EMAIL,     # blue admin card: opens the website on the Pi
}

ENV_FILE = Path(__file__).resolve().parent.parent / "csi" / ".env"


def backend_url():
    if os.getenv("BACKEND_URL"):
        return os.environ["BACKEND_URL"].rstrip("/")
    for line in ENV_FILE.read_text().splitlines() if ENV_FILE.exists() else []:
        if line.startswith("BACKEND_URL="):
            return line.split("=", 1)[1].strip().strip('"').rstrip("/")
    sys.exit(f"Set BACKEND_URL in {ENV_FILE}")


class Client:
    def __init__(self, base):
        self.base = base
        self.http = requests.Session()
        self.http.headers["Origin"] = base   # the server only accepts writes from its own origin

    def post(self, path, body):
        r = self.http.post(f"{self.base}/api/{path}", json=body, timeout=20)
        try:
            data = r.json()
        except ValueError:
            data = {"error": r.text[:200]}
        return r.status_code, data

    def get(self, path):
        r = self.http.get(f"{self.base}/api/{path}", timeout=20)
        r.raise_for_status()
        return r.json()

    def login(self, email, password):
        status, data = self.post("auth/login", {"email": email, "password": password})
        if status != 200:
            sys.exit(f"Sign-in failed for {email}: {data.get('error')}")
        self.http.headers["X-CSRF-Token"] = self.get("auth/me")["csrf"]
        return self


def main():
    base = backend_url()
    print(f"Server: {base}\n")
    admin_pw = getpass.getpass(f"Admin password ({ADMIN_EMAIL}): ")
    teacher_pw = getpass.getpass(f"Teacher password ({TEACHER_EMAIL}): ")
    admin = Client(base).login(ADMIN_EMAIL, admin_pw)
    known = {u["email"]: u for u in admin.get("state")["users"]}

    # 1. Missing student accounts
    missing = [e for e in STUDENTS if e not in known]
    if missing:
        print(f"Creating {len(missing)} student account(s): {', '.join(missing)}")
        new_pw = getpass.getpass("Password for the NEW student accounts (12+ characters): ")
        for address in missing:
            name, roll = STUDENTS[address]
            status, data = Client(base).post("auth/register", {"email": address, "name": name, "password": new_pw,
                                                               "roll_no": roll})
            print(f"  {address}: {'created' if status == 200 else 'NOT created - ' + str(data.get('error'))}")
        known = {u["email"]: u for u in admin.get("state")["users"]}
    else:
        print("All student accounts already exist.")

    # 2. Enroll students in the class (as the teacher)
    teacher = Client(base).login(TEACHER_EMAIL, teacher_pw)
    course = next((c for c in teacher.get("state")["courses"] if c["code"] == CLASS_CODE), None)
    if not course:
        sys.exit(f"Class {CLASS_CODE} not found for {TEACHER_EMAIL}")
    print(f"\nEnrolling students in {CLASS_CODE} ({course['room']}):")
    for address in STUDENTS:
        status, data = teacher.post("roster/add", {"course_id": course["id"], "email": address})
        print(f"  {address}: {'enrolled' if status == 200 else data.get('error')}")

    # 3. Assign cards (as the admin)
    print("\nAssigning cards:")
    for card_uid, address in CARDS.items():
        person = known.get(address)
        if not person:
            print(f"  {card_uid}: no account {address} - skipped")
            continue
        status, data = admin.post("nfc/cards/assign", {"uid": card_uid, "student_id": person["id"]})
        print(f"  {card_uid} -> {person['name']} ({person['role']}): {data.get('message') if status == 200 else data.get('error')}")

    print("\nDone. Test: teacher card (opens session) -> student cards (Present) -> teacher card (closes) -> blue card (website).")


if __name__ == "__main__":
    main()
