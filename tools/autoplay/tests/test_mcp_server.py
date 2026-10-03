"""The MCP server's tools against the fake emulator, `look` against a fake Ollama, and one real
stdio round trip through the MCP SDK when it is installed."""
import asyncio
import base64
import io
import json
import os
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

import kyty_autoplay as ka  # noqa: E402
import vision  # noqa: E402

try:
    from PIL import Image
    import fake_emulator  # noqa: F401  (needs Pillow)
    import mcp_server
    HAVE_PILLOW = True
except ImportError:
    HAVE_PILLOW = False

try:
    import mcp  # noqa: F401
    HAVE_MCP = True
except ImportError:
    HAVE_MCP = False

FAKE = HERE / "fake_emulator.py"


class FakeOllama:
    """Answers /api/chat like Ollama and remembers the last request."""

    def __init__(self, mode="ok"):
        self.mode = mode
        self.requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                owner.requests.append((self.path, body))
                if owner.mode == "missing":
                    payload, code = {"error": f"model \"{body['model']}\" not found, try pulling it first"}, 404
                else:
                    payload, code = {"model": body["model"], "message": {
                        "role": "assistant", "content": "SCREEN: main menu\nPROMPTS: X Accept"}, "done": True}, 200
                data = json.dumps(payload).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def free_port_url():
    import socket
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return f"http://127.0.0.1:{sock.getsockname()[1]}"


class VisionClient(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.png = Path(self.tmp.name) / "s.png"
        # A tiny valid PNG (1x1) that works without Pillow.
        self.png.write_bytes(base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="))

    def tearDown(self):
        self.tmp.cleanup()

    def test_request_and_answer(self):
        ollama = FakeOllama()
        self.addCleanup(ollama.close)
        result = vision.describe(self.png, "Which item is highlighted?", host=ollama.url, model="m:1")
        self.assertEqual(result["answer"], "SCREEN: main menu\nPROMPTS: X Accept")
        self.assertEqual(result["model"], "m:1")
        path, body = ollama.requests[-1]
        self.assertEqual(path, "/api/chat")
        self.assertEqual(body["model"], "m:1")
        self.assertFalse(body["stream"])
        message = body["messages"][0]
        self.assertIn("Which item is highlighted?", message["content"])
        self.assertIn("PROMPTS:", message["content"])
        self.assertTrue(base64.b64decode(message["images"][0]).startswith(b"\x89PNG"))

    def test_missing_model(self):
        ollama = FakeOllama("missing")
        self.addCleanup(ollama.close)
        with self.assertRaises(vision.VisionError) as caught:
            vision.describe(self.png, host=ollama.url, model="qwen2.5vl:3b")
        self.assertIn("ollama pull qwen2.5vl:3b", str(caught.exception))

    def test_unreachable(self):
        with self.assertRaises(vision.VisionError) as caught:
            vision.describe(self.png, host=free_port_url(), timeout=5)
        self.assertIn("not reachable", str(caught.exception))

    @unittest.skipUnless(HAVE_PILLOW, "Pillow is not installed")
    def test_large_screenshots_are_shrunk(self):
        big = Path(self.tmp.name) / "big.png"
        Image.new("RGB", (3840, 2160), (10, 20, 30)).save(big)
        with Image.open(io.BytesIO(base64.b64decode(vision.encode_image(big, 1280)))) as small:
            self.assertEqual(small.size, (1280, 720))


class Aliases(unittest.TestCase):
    def test_xbox_and_dualsense_names(self):
        self.assertEqual(ka.normalize_buttons("RB"), "r1")
        self.assertEqual(ka.normalize_buttons("a+lb"), "cross+l1")
        self.assertEqual(ka.normalize_buttons("cross"), "cross")
        self.assertEqual(ka.normalize_buttons("Start"), "options")
        with self.assertRaises(ka.HarnessError):
            ka.normalize_buttons("jump")
        self.assertEqual(ka.normalize_command("press RB 200"), "press r1 200")
        self.assertEqual(ka.normalize_command("release all"), "release all")
        self.assertEqual(ka.normalize_command("stick l 0 1 500"), "stick l 0 1 500")


@unittest.skipUnless(HAVE_PILLOW, "Pillow is not installed")
class GameToolsAgainstFakeEmulator(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        build = root / "build"
        build.mkdir()
        emulator = build / "kyty_emulator"
        emulator.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{FAKE}" "$@"\n')
        emulator.chmod(0o755)
        self.saved_env = dict(os.environ)
        os.environ.update({"FAKE_BEHAVIOR": "pass", "KYTY_GAME": "/games/fake",
                           "KYTY_AUTOPLAY_STOP_GRACE": "1.5"})
        self.ollama = FakeOllama()
        os.environ["OLLAMA_HOST"] = self.ollama.url
        os.environ["KYTY_VISION_MODEL"] = "fake-vision"
        self.refs = root / "refs"
        self.saved_refs = mcp_server.REFS_DIR
        mcp_server.REFS_DIR = self.refs
        self.tools = mcp_server.GameTools(ka.make_config(build_dir=str(build), root=str(root / "runs")))

    def tearDown(self):
        try:
            if self.tools.run_dir is not None and not (self.tools.run_dir / "exit.json").exists():
                self.tools.stop_game()
        finally:
            mcp_server.REFS_DIR = self.saved_refs
            os.environ.clear()
            os.environ.update(self.saved_env)
            self.ollama.close()
            self.tmp.cleanup()

    def test_navigation_session(self):
        tools = self.tools
        with self.assertRaises(ka.HarnessError):
            tools.press("cross")                      # nothing running yet

        started = tools.start_game(wait_seconds=30)
        self.assertEqual(started["status"], "running", started)
        self.assertIn("already running", tools.start_game()["error"])
        status = tools.game_status()
        self.assertTrue(status["running"])

        self.assertEqual(tools.press("RB", times=2), {"pressed": "r1", "times": 2, "ms": 120})
        commands = (tools.run_dir / "commands.txt").read_text().splitlines()
        self.assertEqual(commands[-2:], ["press r1 120", "press r1 120"])
        tools.press("a+lb", ms=200)
        self.assertEqual((tools.run_dir / "commands.txt").read_text().splitlines()[-1], "press cross+l1 200")
        with self.assertRaises(ka.HarnessError):
            tools.press("jump")
        self.assertEqual(tools.stick("left", 0, 1, 500)["stick"], "l")
        tools.trigger("right", 0.5, 100)
        tools.hold("square")
        tools.release("square")
        tools.send_raw("press X")
        self.assertEqual((tools.run_dir / "commands.txt").read_text().splitlines()[-1], "press square")
        with self.assertRaises(ka.HarnessError):
            tools.send_raw("dance")                   # rejected by the emulator, reported as an error

        shot = tools.screenshot("menu")
        self.assertTrue(shot.is_file())
        self.assertEqual(shot.name, "menu.png")
        again = tools.screenshot("menu")              # same name: a new screenshot, not the old event
        self.assertTrue(again.is_file())

        looked = tools.look("What is selected?")
        self.assertEqual(looked["description"], "SCREEN: main menu\nPROMPTS: X Accept")
        self.assertEqual(looked["model"], "fake-vision")
        self.assertTrue(Path(looked["screenshot"]).is_file())
        _, body = self.ollama.requests[-1]
        self.assertIn("What is selected?", body["messages"][0]["content"])

        made = tools.save_reference("menu box", [0.3, 0.3, 0.7, 0.7])
        self.assertEqual(made["scenario_ref"], "refs/menu_box.png")
        self.assertTrue((self.refs / "menu_box.png").is_file())

        waited = tools.wait(0.5)
        self.assertTrue(waited["running"])
        self.assertGreater(waited["host_presents"], 0)

        stopped = tools.stop_game()
        self.assertEqual(stopped["result"], "STOPPED")
        self.assertTrue(stopped["summary"].endswith("summary.md"))

    def test_look_reports_an_unreachable_ollama(self):
        os.environ["OLLAMA_HOST"] = free_port_url()
        self.tools.start_game(wait_seconds=30)
        looked = self.tools.look()
        self.assertIn("not reachable", looked["error"])
        self.assertTrue(Path(looked["screenshot"]).is_file())

    def test_a_game_that_died_is_reported(self):
        os.environ["FAKE_BEHAVIOR"] = "other_abort"
        started = self.tools.start_game(wait_seconds=30)
        self.assertEqual(started["status"], "exited early")
        self.assertEqual(started["result"], "OTHER_ABORT")
        with self.assertRaises(ka.HarnessError) as caught:
            self.tools.press("cross")
        self.assertIn("no longer running", str(caught.exception))
        status = self.tools.game_status()
        self.assertFalse(status["running"])
        self.assertEqual(status["result"], "OTHER_ABORT")


@unittest.skipUnless(HAVE_MCP and HAVE_PILLOW, "the mcp package or Pillow is not installed")
class StdioRoundTrip(unittest.TestCase):
    def test_initialize_list_and_call(self):
        from mcp import ClientSession
        from mcp.client.stdio import StdioServerParameters, stdio_client

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            build.mkdir()
            emulator = build / "kyty_emulator"
            emulator.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{FAKE}" "$@"\n')
            emulator.chmod(0o755)
            env = {**os.environ, "KYTY_BUILD_DIR": str(build), "KYTY_GAME": "/games/fake",
                   "FAKE_BEHAVIOR": "pass", "KYTY_AUTOPLAY_STOP_GRACE": "1.5"}
            params = StdioServerParameters(command=sys.executable,
                                           args=[str(HERE.parent / "mcp_server.py")], env=env)

            def payload(result):
                text = next(item.text for item in result.content if item.type == "text")
                return json.loads(text)

            async def scenario():
                async with stdio_client(params) as (read, write):
                    async with ClientSession(read, write) as session:
                        await session.initialize()
                        names = {tool.name for tool in (await session.list_tools()).tools}
                        self.assertTrue({"start_game", "stop_game", "press", "look", "screenshot",
                                         "game_status", "save_reference"} <= names)
                        idle = await session.call_tool("game_status", {})
                        self.assertIn("start_game first", payload(idle)["error"])
                        await session.call_tool("start_game", {"wait_seconds": 30})
                        pressed = await session.call_tool("press", {"button": "rb", "times": 2})
                        self.assertEqual(payload(pressed), {"pressed": "r1", "times": 2, "ms": 120})
                        shot = await session.call_tool("screenshot", {"name": "via_mcp"})
                        kinds = [item.type for item in shot.content]
                        self.assertIn("image", kinds)
                        image = next(item for item in shot.content if item.type == "image")
                        self.assertEqual(getattr(image, "mime_type", None) or getattr(image, "mimeType", None), "image/png")
                        self.assertTrue(base64.b64decode(image.data).startswith(b"\x89PNG"))
                        stopped = await session.call_tool("stop_game", {})
                        self.assertEqual(payload(stopped)["result"], "STOPPED")

            asyncio.run(asyncio.wait_for(scenario(), 120))


def launcher_python():
    """The interpreter tools/autoplay/mcp_server.sh would pick."""
    venv = HERE.parent / ".venv" / "bin" / "python"
    return str(venv) if venv.exists() else "python3"


def launcher_has_mcp():
    import subprocess
    try:
        return subprocess.run([launcher_python(), "-c", "import mcp"], capture_output=True).returncode == 0
    except OSError:
        return False


@unittest.skipUnless(HAVE_MCP and launcher_has_mcp(), "the launcher's interpreter has no mcp package")
class Launcher(unittest.TestCase):
    def test_launcher_serves_from_any_directory(self):
        from mcp import ClientSession
        from mcp.client.stdio import StdioServerParameters, stdio_client

        params = StdioServerParameters(command="sh", args=[str(HERE.parent / "mcp_server.sh")],
                                       env=dict(os.environ), cwd=tempfile.gettempdir())

        async def scenario():
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    names = {tool.name for tool in (await session.list_tools()).tools}
                    self.assertIn("look", names)

        asyncio.run(asyncio.wait_for(scenario(), 60))

    def test_launcher_explains_a_missing_sdk(self):
        import subprocess
        with tempfile.TemporaryDirectory() as tmp:
            # A copy of the launcher next to no virtualenv, with a python3 that cannot import mcp.
            launcher = Path(tmp) / "mcp_server.sh"
            launcher.write_text((HERE.parent / "mcp_server.sh").read_text())
            shim = Path(tmp) / "bin"
            shim.mkdir()
            (shim / "python3").write_text(f'#!/bin/sh\nexec "{sys.executable}" -I -S "$@"\n')
            (shim / "python3").chmod(0o755)
            env = {**os.environ, "PATH": f"{shim}:{os.environ.get('PATH', '')}"}
            done = subprocess.run(["sh", str(launcher)], capture_output=True, text=True, env=env,
                                  stdin=subprocess.DEVNULL)
            self.assertEqual(done.returncode, 1)
            self.assertIn("setup-mcp.sh", done.stderr)


if __name__ == "__main__":
    unittest.main()
