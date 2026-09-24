# OpenServiceMeter

A small physical "mood barometer": a box with three buttons (green / yellow / red) and matching LEDs, built on an ESP32. Every press is timestamped, tagged with a device ID and location, and sent to a small REST backend that stores it in PostgreSQL. A Grafana dashboard visualizes the results.

## How it works

- Someone presses **green** ("Good"), **yellow** ("Neutral"), or **red** ("Bad").
- The matching LED blinks twice with a short confirmation chime, and the vote is queued on the device.
- A separate background task on the ESP32 sends the vote to the backend over WiFi/HTTPS as soon as possible; if the network is down, votes stay queued on the device (surviving reboots) and are retried automatically once connectivity returns.
- Pressing a button again within 2 seconds of the last press (accepted or rejected) is treated as an attempt to game the count: the input is rejected with an unpleasant buzzer/LED pattern instead of being recorded, and the lockout resets to a full 2 seconds from that press.
- The backend (FastAPI) validates the request, writes it to PostgreSQL, and optionally logs it to the console — both independently toggled via environment variables.
- A Grafana dashboard reads directly from PostgreSQL and lets you filter by device.

## Hardware

- **Board:** Freenove ESP32-WROVER (any ESP32 dev board with enough free GPIOs works)
- **3× push buttons**, wired to GPIO with the ESP32's internal pull-ups (button connects the pin to GND when pressed)
- **3× LEDs**, optionally switched via a transistor/MOSFET per LED
- **1× passive buzzer**, driven with a manually generated square wave
- **Power:** USB-C into the ESP32

| Signal        | GPIO |
|---------------|------|
| Button green  | 32   |
| Button yellow | 33   |
| Button red    | 25   |
| LED green     | 26   |
| LED yellow    | 27   |
| LED red       | 14   |
| Buzzer        | 4    |

## Firmware

Built with [PlatformIO](https://platformio.org/) (VS Code extension).

### Setup

1. Copy `include/config.h.example` to `include/config.h` and fill in your real values:
   ```cpp
   #define WIFI_SSID     "YOUR_WIFI_NAME"
   #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
   #define API_ENDPOINT  "https://your-domain.example/openservicemeter/vote"
   #define API_KEY       "one-of-the-keys-from-your-backend-.env"
   #define DEVICE_ID     "barometer-01"   // must match the device_id mapped to API_KEY in the backend's API_KEYS
   #define LOCATION      "Reception"
   ```
2. Build and flash:
   ```
   pio run --target upload
   ```
3. Open the serial monitor (115200 baud) to watch WiFi connection, queueing and send status:
   ```
   pio device monitor
   ```

### Firmware behavior

- **Dual-core architecture:** button/LED/buzzer handling runs in `loop()` on Core 1 and never touches the network. All WiFi, NTP and HTTP work happens in a dedicated FreeRTOS task pinned to Core 0. This means a slow or unreachable backend can never delay button feedback or the anti-spam lockout.
- **Offline queue:** every vote is first written to a small file in LittleFS (`/queue.jsonl`), then picked up by the network task. If sending fails, the vote stays queued and is retried; if the ESP32 reboots (power loss) with unsent votes still queued, they're sent on the next boot and marked `"queued": true` in the payload.
- **Retry backoff:** after a failed send, the network task waits 60 seconds before retrying (instead of hammering the API every poll cycle). A successful send immediately resumes normal polling.
- **Anti-manipulation lockout:** any button press starts (or resets) a 2-second lockout. A press during an active lockout is rejected (harsh buzzer + flashing LEDs) and resets the lockout to a fresh 2 seconds from that moment — the lockout never exceeds 2 seconds, no matter how many times someone mashes the buttons.

## Backend

FastAPI app, deployed via Docker Compose alongside a dedicated PostgreSQL instance.

### Setup

1. Copy `.env.example` to `.env` (next to your `docker-compose.yml`) and fill in real values:
   ```
   POSTGRES_USER=openservicemeter
   POSTGRES_PASSWORD=<a real password>
   POSTGRES_DB=openservicemeter

   # Format: key1:device_id1,key2:device_id2,...
   API_KEYS=<real-api-key-1>:barometer-01,<real-api-key-2>:barometer-02

   # External port the API is reachable on (internally always port 8000)
   API_PORT=8091
   ```
2. Merge `backend/docker-compose.snippet.yml` into your own `docker-compose.yml` (or use it as-is if this is a standalone deployment). It defines two services, `api` and `db` — the service name `db` is also the hostname the API uses to reach Postgres, so if you rename it, update `PG_CONN_STR` in the snippet to match.
3. Start it:
   ```
   docker compose up -d --build api db
   ```
4. Create the table once:
   ```sql
   CREATE TABLE openservicemeter_votes (
       id SERIAL PRIMARY KEY,
       device_id VARCHAR(50) NOT NULL,
       location VARCHAR(100) NOT NULL,
       value VARCHAR(10) NOT NULL,
       device_timestamp TIMESTAMPTZ NOT NULL,
       received_at TIMESTAMPTZ NOT NULL,
       queued BOOLEAN NOT NULL DEFAULT FALSE
   );
   ```
   ```
   docker compose exec db psql -U openservicemeter -d openservicemeter
   ```
5. Put a reverse proxy (Caddy, nginx, ...) in front of the API for TLS if it's reachable from outside your local network.

### Environment variables

| Variable            | Purpose                                                              |
|----------------------|-----------------------------------------------------------------------|
| `LOG_ENABLED`         | `true`/`false` — log every received vote to the container's console  |
| `SQL_WRITE_ENABLED`   | `true`/`false` — write every received vote to PostgreSQL             |
| `PG_CONN_STR`         | PostgreSQL connection string (only required if `SQL_WRITE_ENABLED`)  |
| `API_KEYS`            | `key1:device_id1,key2:device_id2,...` — one API key per device       |

### API

- **`POST /vote`** — records a vote.
  - Header: `X-Api-Key: <key from API_KEYS>`
  - Body:
    ```json
    {
      "device_id": "meter-01",
      "location": "Reception",
      "value": "green",
      "timestamp": "2026-09-24T10:11:51Z",
      "queued": false
    }
    ```
  - `value` must be `green`, `yellow`, or `red`.
  - Returns `401` for an unknown API key, `403` if the key doesn't match the given `device_id`, `422` for a malformed body.
- **`GET /health`** — returns `{"status": "up", "log_enabled": ..., "sql_write_enabled": ...}`.

## Grafana dashboard

1. Add a **PostgreSQL** data source in Grafana pointing at your `db` service (host = your Docker host's IP or hostname, port `5432`, database/user/password from your `.env`).

   > The dashboard JSON references the data source **by name**. Make sure the data source is named exactly `openservicemeter`, or edit the `"datasource"` fields in the JSON to match whatever name you gave it.

2. **Dashboards → Import**, upload `openservicemeter-grafana-dashboard.json`
3. The dashboard includes:
   - A **device filter** variable (multi-select + "All"), sourced from the distinct `device_id` values in the table.
   - A **donut chart** of vote totals by rating, colored green/yellow/red.
   - A **stacked bar chart** of votes over time, split by rating.
   - A **raw votes table** with color-coded rating cells.

## License

Licensed under the PolyForm Noncommercial License 1.0.0 — free to use, modify, and contribute to for any noncommercial purpose. **Commercial use requires prior written permission from Paul Hack.** See LICENSE for the full terms and how to request a commercial license.