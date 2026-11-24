"""Vercel Python function: shares the exact local API implementation."""
import logging

from backend.database import make_engine, migrate
from backend.service import Service
from backend.server import Handler

_engine = make_engine()
try:
    # Idempotent: creates any missing tables (additive schema) on cold start.
    migrate(_engine)
    from backend.bootstrap import bootstrap_admin
    bootstrap_admin(_engine)
except Exception:  # a concurrent cold start may race on CREATE TABLE; next start retries
    logging.exception('Schema setup on cold start failed')


class handler(Handler):
    service = Service(_engine)
