# clumsy HTTP API

This document describes the embedded HTTP API used to remotely control `clumsy`.

## Overview

- Protocol: plain HTTP
- Default bind/port: configured from `scenarios.ini`
- Content type: `application/json`

The HTTP server reads its configuration from the `[server]` section in `scenarios.ini`.

Example:

```ini
[server]
bind = 0.0.0.0
port = 7878
```

## Endpoints

### `GET /health`

Returns the current runtime status.

Example response:

```json
{
  "server": "running",
  "filtering": true,
  "activeScenario": "lag_tcp"
}
```

If no scenario is currently armed:

```json
{
  "server": "running",
  "filtering": false,
  "activeScenario": null
}
```

### `GET /scenarios`

Returns the names of all scenarios loaded from `scenarios.ini`.

Example response:

```json
{
  "scenarios": ["drop_udp", "lag_tcp"]
}
```

### `POST /scenarios/{name}/arm`

Arms the named scenario.

Behavior:

- stops any current filtering session
- applies the scenario values to the live UI/runtime state
- starts filtering with that scenario
- marks the scenario as active

Example request:

```http
POST /scenarios/lag_tcp/arm HTTP/1.1
Host: 127.0.0.1:7878
```

Success response:

```json
{
  "ok": true,
  "activeScenario": "lag_tcp"
}
```

Common errors:

- `400 Bad Request`

```json
{"ok":false,"error":"Malformed arm path."}
```

- `404 Not Found`

```json
{"ok":false,"error":"Scenario not found: lag_tcp"}
```

- `409 Conflict`

Returned when the scenario was found but filtering could not be started, for example due to an invalid filter or WinDivert startup failure.

Example:

```json
{"ok":false,"error":"Failed to start filtering : filter syntax error."}
```

- `503 Service Unavailable`

Returned when the UI thread is busy or the server cannot complete the request in time.

### `POST /disarm`

Disarms the current scenario.

Behavior:

- stops filtering if it is running
- clears the active scenario
- leaves the last applied values visible in the UI

Example request:

```http
POST /disarm HTTP/1.1
Host: 127.0.0.1:7878
```

Success response:

```json
{
  "ok": true,
  "activeScenario": null
}
```

## Error responses

Errors are returned as JSON in this format:

```json
{
  "ok": false,
  "error": "Human-readable message"
}
```

Typical status codes:

- `400 Bad Request`
- `404 Not Found`
- `405 Method Not Allowed`
- `409 Conflict`
- `500 Internal Server Error`
- `503 Service Unavailable`

## Notes

- The server handles one request per connection.
- Keep-alive and chunked request bodies are not supported.
- Scenario names in the URL may be URL-encoded.
- The HTTP server can be started or stopped from the UI.
- When the HTTP server is running, the scenario-editing UI is locked, but the main Start/Stop button remains available as a manual override.
