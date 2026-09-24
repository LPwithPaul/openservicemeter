from fastapi import FastAPI, Header, HTTPException
from pydantic import BaseModel, Field
from datetime import datetime, timezone
import os

app = FastAPI()

# --- ENV toggles ---
LOG_ENABLED = os.environ.get("LOG_ENABLED", "true").lower() == "true"
SQL_WRITE_ENABLED = os.environ.get("SQL_WRITE_ENABLED", "false").lower() == "true"

# One API key per device, so individual boxes can be revoked.
# Format of the API_KEYS env variable: "key1:device_id1,key2:device_id2,..."
def parse_api_keys(raw: str) -> dict[str, str]:
    keys: dict[str, str] = {}
    for entry in raw.split(","):
        entry = entry.strip()
        if not entry:
            continue
        key, sep, device_id = entry.partition(":")
        if not sep:
            raise ValueError(f"Invalid API_KEYS entry (no ':' found): {entry!r}")
        keys[key] = device_id
    if not keys:
        raise ValueError("API_KEYS is empty or not set")
    return keys


API_KEYS = parse_api_keys(os.environ["API_KEYS"])

# Only import/connect psycopg2 if SQL write is actually enabled -
# only then is PG_CONN_STR required.
if SQL_WRITE_ENABLED:
    import psycopg2
    PG_CONN_STR = os.environ["PG_CONN_STR"]

    def get_connection():
        return psycopg2.connect(PG_CONN_STR)


class Vote(BaseModel):
    device_id: str
    location: str
    value: str = Field(pattern="^(gruen|gelb|rot)$")
    timestamp: str
    queued: bool = False  # true = delivered by the ESP from its retry queue, not a live click


def log_vote(vote: Vote, received_at: datetime, db_written: bool):
    if not LOG_ENABLED:
        return
    print(
        f"[VOTE] {received_at.isoformat()} device={vote.device_id} "
        f"location={vote.location} value={vote.value} "
        f"device_ts={vote.timestamp} queued={vote.queued} db_written={db_written}",
        flush=True,
    )


@app.post("/vote")
def receive_vote(vote: Vote, x_api_key: str = Header(...)):
    if x_api_key not in API_KEYS:
        raise HTTPException(status_code=401, detail="Invalid API key")
    if API_KEYS[x_api_key] != vote.device_id:
        raise HTTPException(status_code=403, detail="API key does not match device_id")

    try:
        device_ts = datetime.fromisoformat(vote.timestamp.replace("Z", "+00:00"))
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid timestamp format")

    received_at = datetime.now(timezone.utc)
    db_written = False

    if SQL_WRITE_ENABLED:
        conn = get_connection()
        cursor = conn.cursor()
        cursor.execute(
            """
            INSERT INTO openservicemeter_votes
                (device_id, location, value, device_timestamp, received_at, queued)
            VALUES (%s, %s, %s, %s, %s, %s)
            """,
            (vote.device_id, vote.location, vote.value, device_ts, received_at, vote.queued),
        )
        conn.commit()
        cursor.close()
        conn.close()
        db_written = True

    log_vote(vote, received_at, db_written)

    return {"status": "ok", "db_written": db_written}


@app.get("/health")
def health():
    return {
        "status": "up",
        "log_enabled": LOG_ENABLED,
        "sql_write_enabled": SQL_WRITE_ENABLED,
    }