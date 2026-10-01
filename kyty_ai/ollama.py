from __future__ import annotations

import json
import urllib.request
import urllib.error
from typing import Any


class OllamaError(RuntimeError):
    pass


def generate(
    *,
    url: str,
    model: str,
    prompt: str,
    timeout: int = 900,
    temperature: float = 0.1,
) -> str:
    endpoint = url.rstrip("/") + "/api/generate"
    payload: dict[str, Any] = {
        "model": model,
        "prompt": prompt,
        "stream": False,
        "options": {
            "temperature": temperature,
        },
    }

    body = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        endpoint,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read().decode("utf-8", errors="replace")
    except (urllib.error.URLError, TimeoutError) as exc:
        raise OllamaError(f"Could not contact Ollama at {endpoint}: {exc}") from exc

    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise OllamaError(f"Ollama returned invalid JSON: {raw[:1000]}") from exc

    if data.get("error"):
        raise OllamaError(str(data["error"]))

    return str(data.get("response", ""))
