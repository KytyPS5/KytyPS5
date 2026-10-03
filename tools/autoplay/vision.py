"""Ask a local Ollama vision model what a game screenshot shows.

Standard library only; Pillow, when installed, shrinks the screenshot before it is sent.

    OLLAMA_HOST             default http://localhost:11434
    KYTY_VISION_MODEL       default qwen2.5vl:3b (small enough to share an 8 GB GPU with the game)
    KYTY_VISION_MAX_WIDTH   default 1280
    KYTY_VISION_TIMEOUT     seconds, default 120
    KYTY_VISION_NUM_CTX     default 8192 (image tokens count against this; Ollama otherwise clips the prompt)
    KYTY_VISION_NUM_PREDICT default 512
"""
from __future__ import annotations

import base64
import io
import json
import os
import re
import time
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_MODEL = "qwen2.5vl:3b"
DEFAULT_HOST = "http://localhost:11434"
DEFAULT_NUM_CTX = 8192
DEFAULT_NUM_PREDICT = 512
# prompt_eval_count this close to num_ctx means Ollama dropped the front of the prompt.
TRUNCATION_MARGIN = 8

SCREENS = ("black", "loading", "logo/intro", "legal text", "main menu", "pause menu",
           "dialog box", "cutscene", "free gameplay", "error", "unsure")
STATES = ("controllable", "waiting", "loading", "error", "unsure")

NAVIGATION_PROMPT = f"""Describe this GTA V screenshot as JSON. Use "unsure" when you cannot tell. Do not guess.
screen: {", ".join(SCREENS)}
text: readable text, most prominent first, or ""
selected: the highlighted item, "none", or "unsure"
prompts: button prompts such as "X Accept, O Back", "none", or "unsure"
state: {", ".join(STATES)} (controllable means the radar is in the bottom-left)
answer: the reply to the question below, or "" """


class VisionError(Exception):
    """Ollama could not answer; the message says what to do about it."""


def settings() -> dict:
    return {
        "host": os.environ.get("OLLAMA_HOST", DEFAULT_HOST).rstrip("/"),
        "model": os.environ.get("KYTY_VISION_MODEL", DEFAULT_MODEL),
        "max_width": int(os.environ.get("KYTY_VISION_MAX_WIDTH", "1280")),
        "timeout": float(os.environ.get("KYTY_VISION_TIMEOUT", "120")),
        "num_ctx": int(os.environ.get("KYTY_VISION_NUM_CTX", DEFAULT_NUM_CTX)),
        "num_predict": int(os.environ.get("KYTY_VISION_NUM_PREDICT", DEFAULT_NUM_PREDICT)),
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


def _json_object(text: str) -> dict | None:
    text = re.sub(r"^```(?:json)?\s*|\s*```$", "", text.strip(), flags=re.IGNORECASE | re.DOTALL).strip()
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        start, end = text.find("{"), text.rfind("}")
        if start < 0 or end <= start:
            return None
        try:
            data = json.loads(text[start:end + 1])
        except json.JSONDecodeError:
            return None
    return data if isinstance(data, dict) else None


def _text(value: object) -> str:
    if isinstance(value, str):
        return value.strip()
    if isinstance(value, list):
        return ", ".join(str(item).strip() for item in value if str(item).strip())
    return ""


def _choice(value: object, allowed: tuple[str, ...]) -> str:
    text = _text(value).lower()
    return text if text in allowed else "unsure"


def _field(data: dict, name: str) -> object:
    for key, value in data.items():
        if str(key).lower() == name:
            return value
    return None


def parse_description(content: str) -> dict:
    """The model's JSON, or screen "unsure" plus a short raw snippet when it is not JSON."""
    data = _json_object(content)
    if data is None:
        return {"screen": "unsure", "text": "", "selected": "unsure", "prompts": "unsure",
                "state": "unsure", "answer": "", "raw": content.strip()[:300]}
    selected = _text(_field(data, "selected"))
    prompts = _text(_field(data, "prompts"))
    return {
        "screen": _choice(_field(data, "screen"), SCREENS),
        "text": _text(_field(data, "text")),
        "selected": selected or "unsure",
        "prompts": prompts or "unsure",
        "state": _choice(_field(data, "state"), STATES),
        "answer": _text(_field(data, "answer")),
    }


def describe(image: Path, question: str = "", *, model: str | None = None, host: str | None = None,
             timeout: float | None = None, max_width: int | None = None) -> dict:
    """Send a screenshot to Ollama and return the parsed screen description."""
    config = settings()
    model = model or config["model"]
    host = (host or config["host"]).rstrip("/")
    timeout = timeout if timeout is not None else config["timeout"]
    max_width = max_width if max_width is not None else config["max_width"]
    num_ctx = config["num_ctx"]
    num_predict = config["num_predict"]

    prompt = NAVIGATION_PROMPT
    if question.strip():
        prompt += f"\nQuestion: {question.strip()}"
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt, "images": [encode_image(image, max_width)]}],
        "stream": False,
        "format": "json",
        "options": {"temperature": 0, "num_ctx": num_ctx, "num_predict": num_predict},
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
    parsed = parse_description(answer)
    counted = reply.get("prompt_eval_count")
    parsed["truncated"] = reply.get("done_reason") == "length" or (
        isinstance(counted, int) and num_ctx - counted <= TRUNCATION_MARGIN)
    parsed["model"] = model
    parsed["seconds"] = round(time.monotonic() - started, 1)
    return parsed
