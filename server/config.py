"""Configuration and logging setup.

Reads values from `.env` via python-dotenv. The rest of the code imports
constants from this module instead of reading the environment directly.
"""

import logging
import logging.handlers
import os
import secrets
from pathlib import Path

from dotenv import load_dotenv

BASE_DIR = Path(__file__).resolve().parent

load_dotenv(BASE_DIR / ".env")


def _env(name: str, default: str = "") -> str:
    value = os.environ.get(name, default)
    return value.strip() if value is not None else default


def _env_int(name: str, default: int) -> int:
    raw = _env(name, str(default))
    try:
        return int(raw)
    except ValueError:
        return default


ADMIN_PASSWORD = _env("ADMIN_PASSWORD", "admin123")

# SECRET_KEY must be stable across restarts so existing sessions survive a
# service reload. We fall back to a generated value only for dev convenience.
SECRET_KEY = _env("SECRET_KEY") or secrets.token_hex(32)

_db_raw = _env("DB_PATH", "football.db")
DB_PATH = _db_raw if os.path.isabs(_db_raw) else str(BASE_DIR / _db_raw)

MQTT_HOST = _env("MQTT_HOST", "localhost")
MQTT_PORT = _env_int("MQTT_PORT", 1883)

LOG_LEVEL = _env("LOG_LEVEL", "INFO").upper()
MAX_SCORE = _env_int("MAX_SCORE", 10)
PICO_OFFLINE_SEC = _env_int("PICO_OFFLINE_SEC", 30)


def configure_logging() -> None:
    """Set up a rotating file logger plus a console handler."""
    log_dir = BASE_DIR / "logs"
    log_dir.mkdir(exist_ok=True)

    fmt = logging.Formatter(
        "%(asctime)s %(levelname)s %(module)s: %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    root = logging.getLogger()
    root.setLevel(getattr(logging, LOG_LEVEL, logging.INFO))

    # Clear any prior handlers (matters when Flask reloads in debug mode).
    for h in list(root.handlers):
        root.removeHandler(h)

    file_handler = logging.handlers.RotatingFileHandler(
        log_dir / "server.log",
        maxBytes=10 * 1024 * 1024,
        backupCount=5,
        encoding="utf-8",
    )
    file_handler.setFormatter(fmt)
    root.addHandler(file_handler)

    console = logging.StreamHandler()
    console.setFormatter(fmt)
    root.addHandler(console)
