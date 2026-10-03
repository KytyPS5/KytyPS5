"""Ask a local Ollama vision model what a game screenshot shows.

Standard library only; Pillow, when installed, shrinks the screenshot before it is sent.

    OLLAMA_HOST          default http://localhost:11434
    KYTY_VISION_MODEL    default qwen2.5vl:3b (small enough to share an 8 GB GPU with the game)
    KYTY_VISION_MAX_WIDTH  default 1280
    KYTY_VISION_TIMEOUT  seconds, default 120
"""
from __future__ import annotations

import base64
import io
import json
import os
import time
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_MODEL = "qwen2.5vl:3b"
DEFAULT_HOST = "http://localhost:11434"

NAVIGATION_PROMPT = """You are looking at one screenshot of a video game (Grand Theft Auto V, running in a \
PlayStation 5 emulator). Someone who cannot see the screen will decide which controller button to press \
from your answer, so be literal and only report what is visible. Answer in exactly this format:

SCREEN: what kind of screen this is (black, loading, logo/intro, legal text, main menu, pause menu, \
dialog box, cutscene, free gameplay, error) and anything distinctive
TEXT: the readable text on screen, most prominent first (titles, menu items, messages)
SELECTED: the highlighted or selected menu item, or "none"
PROMPTS: button prompts shown on screen and what they do, e.g. "X Accept, O Back", or "none"
STATE: is the player character visible and controllable (minimap/radar in the bottom-left corner), \
or is the game waiting for input, loading, or showing an error"""


class VisionError(Exception):
    """Ollama could not answer; the message says what to do about it."""


def settings() -> dict:
    return {
        "host": os.environ.get("OLLAMA_HOST", DEFAULT_HOST).rstrip("/"),
        "model": os.environ.get("KYTY_VISION_MODEL", DEFAULT_MODEL),
        "max_width": int(os.environ.get("KYTY_VISION_MAX_WIDTH", "1280")),
        "timeout": float(os.environ.get("KYTY_VISION_TIMEOUT", "120")),
    }


def encode_image(path: Path, max_width: int) -> str:
    """The screenshot as base64 PNG, shrunk to max_width when Pillow is available."""
    data = path.read_bytes()
    try:
        from PIL import Image  # type: ignore
    except ImportError:
        return base64.b64encode(data).decode()
    with Image.open(io.BytesIO(data)) as image:
        if max_width > 0 and image.width > max_width:
            height = max(1, round(image.height * max_width / image.width))
            image = image.convert("RGB").resize((max_width, height), Image.LANCZOS)
            out = io.BytesIO()
            image.save(out, format="PNG")
            data = out.getvalue()
    return base64.b64encode(data).decode()


def describe(image: Path, question: str = "", *, model: str | None = None, host: str | None = None,
             timeout: float | None = None, max_width: int | None = None) -> dict:
    """Send a screenshot to Ollama and return {"answer", "model", "seconds"}."""
    config = settings()
    model = model or config["model"]
    host = (host or config["host"]).rstrip("/")
    timeout = timeout if timeout is not None else config["timeout"]
    max_width = max_width if max_width is not None else config["max_width"]

    prompt = NAVIGATION_PROMPT
    if question.strip():
        prompt += f"\n\nThen answer this question about the screenshot:\n{question.strip()}"
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt, "images": [encode_image(image, max_width)]}],
        "stream": False,
        "options": {"temperature": 0},
    }).encode()
    request = urllib.request.Request(f"{host}/api/chat", data=body,
                                     headers={"Content-Type": "application/json"})
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            reply = json.loads(response.read())
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")
        if error.code == 404 or "not found" in detail.lower():
            raise VisionError(f"Ollama has no model {model!r}: run `ollama pull {model}` "
                              f"(or set KYTY_VISION_MODEL)") from None
        raise VisionError(f"Ollama returned HTTP {error.code}: {detail.strip()[:300]}") from None
    except (urllib.error.URLError, ConnectionError) as error:
        reason = getattr(error, "reason", error)
        raise VisionError(f"Ollama is not reachable at {host} ({reason}); start it with `ollama serve` "
                          f"or set OLLAMA_HOST") from None
    except TimeoutError:
        raise VisionError(f"Ollama did not answer within {timeout:.0f} s; the model may not fit in "
                          f"memory next to the game (check `ollama ps`)") from None
    except ValueError:
        raise VisionError("Ollama returned something that is not JSON") from None
    if "error" in reply:
        raise VisionError(f"Ollama: {reply['error']}")
    answer = (reply.get("message") or {}).get("content", "").strip()
    if not answer:
        raise VisionError("Ollama returned an empty answer")
    return {"answer": answer, "model": model, "seconds": round(time.monotonic() - started, 1)}
