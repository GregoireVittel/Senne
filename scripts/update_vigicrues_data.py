#!/usr/bin/env python3
"""Fetch Vigicrues data used by the static GitHub Pages app.

GitHub Pages browsers cannot reliably fetch vigicrues.gouv.fr directly because
that API does not send CORS headers. This script runs server-side in GitHub
Actions and writes same-origin data.json for the page.
"""

from __future__ import annotations

import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

STATION = "F712000102"
API_BASE = "https://www.vigicrues.gouv.fr/services"
OBS_URL = f"{API_BASE}/observations.json/index.php?CdStationHydro={STATION}&GrdSerie=H&FormatDate=iso"
STATION_URL = f"{API_BASE}/station.json/index.php?CdStationHydro={STATION}"
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "data.json"


class TransientVigicruesError(RuntimeError):
    """The upstream service was unavailable; retain the last known-good data."""


def fetch_json(url: str, attempts: int = 4) -> object:
    """Fetch a JSON resource, retrying only short-lived upstream failures."""
    last_error: Exception | None = None
    for attempt in range(1, attempts + 1):
        try:
            req = urllib.request.Request(
                url, headers={"User-Agent": "Senne-GitHub-Pages-Updater/1.1"}
            )
            with urllib.request.urlopen(req, timeout=30) as response:
                if response.status != 200:
                    raise RuntimeError(f"HTTP {response.status} while fetching {url}")
                return json.load(response)
        except urllib.error.HTTPError as error:
            last_error = error
            transient = error.code in {408, 429, 500, 502, 503, 504}
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            last_error = error
            transient = True
        except RuntimeError:
            raise

        if not transient:
            raise RuntimeError(
                f"Vigicrues request failed after {attempt} attempt(s): {last_error}"
            ) from last_error
        if attempt == attempts:
            raise TransientVigicruesError(
                f"Vigicrues request failed after {attempt} attempt(s): {last_error}"
            ) from last_error
        delay = 2 * attempt
        print(
            f"Vigicrues request attempt {attempt}/{attempts} failed ({last_error}); "
            f"retrying in {delay}s",
            file=sys.stderr,
        )
        time.sleep(delay)

    raise AssertionError("unreachable")


def observation_count(observations: object) -> int:
    if not isinstance(observations, dict):
        return 0
    serie = observations.get("Serie")
    if not isinstance(serie, dict):
        return 0
    raw = serie.get("ObssHydro")
    return len(raw) if isinstance(raw, list) else 0


def main() -> int:
    try:
        observations = fetch_json(OBS_URL)
        station = fetch_json(STATION_URL)
    except TransientVigicruesError as error:
        # A stale but valid same-origin data.json is safer than failing the scheduled
        # job (and creating an alert) while Vigicrues has a temporary outage.
        print(f"Vigicrues temporarily unavailable; retained existing data.json: {error}")
        return 0

    count = observation_count(observations)
    if count <= 0:
        raise RuntimeError("Vigicrues returned no observation data")

    payload = {
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "stationCode": STATION,
        "sources": {"observations": OBS_URL, "station": STATION_URL},
        "observations": observations,
        "station": station,
    }
    OUT.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {OUT} with {count} observations")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # CLI should print concise failure
        print(f"update_vigicrues_data failed: {exc}", file=sys.stderr)
        raise
