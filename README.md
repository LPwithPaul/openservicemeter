Starten:

docker compose up -d --build openservicemeter-api openservicemeter-db

Tabelle Anlegen:

   docker compose exec openservicemeter-db psql -U openservicemeter -d openservicemeter

   CREATE TABLE openservicemeter_votes (
    id SERIAL PRIMARY KEY,
    device_id VARCHAR(50) NOT NULL,
    location VARCHAR(100) NOT NULL,
    value VARCHAR(10) NOT NULL,
    device_timestamp TIMESTAMPTZ NOT NULL,
    received_at TIMESTAMPTZ NOT NULL,
    queued BOOLEAN NOT NULL DEFAULT FALSE
);