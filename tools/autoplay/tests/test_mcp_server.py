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


OK_SCREEN = {"screen": "main menu", "text": "Story", "selected": "Story",
             "prompts": "X Accept", "state": "waiting", "answer": "Story"}


class FakeOllama:
    """Answers /api/chat like Ollama and remembers the last request."""

    def __init__(self, mode="ok", prompt_eval_count=100):
        self.mode = mode
        self.prompt_eval_count = prompt_eval_count
        self.requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                owner.requests.append((self.path, body))
                if owner.mode == "missing":
                    payload, code = {"error": f"model \"{body['model']}\" not found, try pulling it first"}, 404
                else:
                    content = "not json" if owner.mode == "garbage" else json.dumps(OK_SCREEN)
                    payload, code = {"model": body["model"], "message": {
                        "role": "assistant", "content": content}, "done": True,
                        "done_reason": "length" if owner.mode == "length" else "stop",
                        "prompt_eval_count": owner.prompt_eval_count}, 200
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
        self.assertEqual(result["screen"], "main menu")
        self.assertEqual(result["answer"], "Story")
        self.assertFalse(result["truncated"])
        self.assertEqual(result["model"], "m:1")
        path, body = ollama.requests[-1]
        self.assertEqual(path, "/api/chat")
        self.assertEqual(body["model"], "m:1")
        self.assertEqual(body["format"], "json")
        self.assertFalse(body["stream"])
        self.assertEqual(body["options"]["num_ctx"], 8192)
        self.assertEqual(body["options"]["num_predict"], 512)
        message = body["messages"][0]
        self.assertIn("Which item is highlighted?", message["content"])
        self.assertIn("unsure", message["content"])
        self.assertTrue(base64.b64decode(message["images"][0]).startswith(b"\x89PNG"))

    def test_a_cut_off_reply_is_marked_truncated(self):
        ollama = FakeOllama("length")
        self.addCleanup(ollama.close)
        result = vision.describe(self.png, host=ollama.url, model="m:1")
        self.assertTrue(result["truncated"])
        self.assertEqual(result["screen"], "main menu")

    def test_a_prompt_that_fills_the_context_is_truncated(self):
        ollama = FakeOllama(prompt_eval_count=8188)
        self.addCleanup(ollama.close)
        result = vision.describe(self.png, host=ollama.url, model="m:1")
        self.assertTrue(result["truncated"])

    def test_non_json_is_unsure(self):
        ollama = FakeOllama("garbage")
        self.addCleanup(ollama.close)
        result = vision.describe(self.png, host=ollama.url, model="m:1")
        self.assertEqual(result["screen"], "unsure")
        self.assertIn("not json", result["raw"])

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

        early = tools.summary()
        self.assertIn("still running", early["error"])

        looked = tools.look("What is selected?")
        self.assertEqual(looked["screen"], "main menu")
        self.assertEqual(looked["prompts"], "X Accept")
        self.assertEqual(looked["state"], "waiting")
        self.assertEqual(looked["model"], "fake-vision")
        self.assertFalse(looked["truncated"])
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
        report = tools.summary()
        self.assertTrue(report["markdown"].startswith("# STOPPED"))

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

    def test_shader_capture_downloads_as_a_zip(self):
        import zipfile
        fixture = HERE.parents[2] / "tests" / "data" / "shader_capture" / "gpu_selected_store"
        fatal = Path(self.tmp.name) / "fatal.txt"
        fatal.write_text("--- Build ---\nSource build test\n--- Error ---\n"
                         "shader resource tracking: hash=0x00000000000000b2 stage=compute pc=0x10 "
                         "buffer descriptor is not a valid runtime value in /tmp/x.cpp:1\n")
        os.environ["FAKE_BEHAVIOR"] = "shader_abort"
        os.environ["FAKE_FATAL_FILE"] = str(fatal)
        os.environ["FAKE_CAPTURE_DIR"] = str(fixture)
        started = self.tools.start_game(wait_seconds=30)
        self.assertEqual(started["result"], "SHADER_ABORT", started)
        got = self.tools.shader()
        self.assertEqual(got["name"], "cs_00000000000000b2_00000000")
        self.assertIn("manifest.json", got["files"])
        self.assertIn("code.bin", got["files"])
        with zipfile.ZipFile(io.BytesIO(base64.b64decode(got["zip_base64"]))) as archive:
            self.assertEqual(json.loads(archive.read("manifest.json"))["hash"], "0x00000000000000b2")
            self.assertTrue(archive.read("code.bin"))
        self.assertIn("no capture", self.tools.shader("../secret")["error"])
        saved = mcp_server.MAX_SHADER_ZIP
        mcp_server.MAX_SHADER_ZIP = 1
        try:
            over = self.tools.shader()
        finally:
            mcp_server.MAX_SHADER_ZIP = saved
        self.assertIn("16 MB", over["error"])
        self.assertGreater(over["bytes"], 1)
        self.assertNotIn("zip_base64", over)


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
                                         "game_status", "save_reference", "summary", "shader"} <= names)
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


@unittest.skipUnless(HAVE_MCP, "the mcp package is not installed")
class HttpServer(unittest.TestCase):
    """--http: a per-start key guards every request, and a tunnel's Host header is accepted."""

    def setUp(self):
        import re
        import socket
        import subprocess
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.build = tempfile.TemporaryDirectory()
        self.addCleanup(self.build.cleanup)
        env = {**os.environ, "KYTY_BUILD_DIR": self.build.name}
        self.proc = subprocess.Popen([sys.executable, str(HERE.parent / "mcp_server.py"), "--http",
                                      "--port", str(self.port)],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=env)
        self.addCleanup(self.stop)
        banner = ""
        # The banner ends with the closing brace of the .mcp.json example.
        while not banner.endswith("\n}\n"):
            line = self.proc.stderr.readline()
            if not line:
                self.fail(f"server exited before printing its key:\n{banner}")
            banner += line
        self.banner = banner
        self.key = re.search(r"^\s{6}(\S{40,})$", banner, re.M).group(1)
        self.url = f"http://127.0.0.1:{self.port}/mcp"
        self.wait_listening()

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()

    def wait_listening(self):
        import socket
        import time
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.1)
        self.fail("server never listened")

    def post(self, headers):
        import urllib.error
        import urllib.request
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "test", "version": "0"}}}).encode()
        request = urllib.request.Request(self.url, data=body, headers={
            "Content-Type": "application/json", "Accept": "application/json, text/event-stream", **headers})
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status
        except urllib.error.HTTPError as error:
            return error.code

    def test_key_is_new_on_every_start_and_shown_with_connection_help(self):
        self.assertIn("cloudflared tunnel --url", self.banner)
        self.assertIn(f"Authorization: Bearer {self.key}", self.banner)
        first = self.key
        self.stop()
        self.setUp()
        self.assertNotEqual(first, self.key)

    def test_requests_without_the_key_are_refused(self):
        self.assertEqual(self.post({}), 401)
        self.assertEqual(self.post({"Authorization": "Bearer wrong"}), 401)
        self.assertEqual(self.post({"Authorization": self.key}), 401)          # no "Bearer "

    def test_a_tunnel_hostname_is_accepted_with_the_key(self):
        status = self.post({"Authorization": f"Bearer {self.key}", "Host": "kyty.example.trycloudflare.com"})
        self.assertEqual(status, 200)

    def test_mcp_client_with_the_key(self):
        from mcp import ClientSession
        from mcp.client.streamable_http import streamable_http_client
        from mcp.shared._httpx_utils import create_mcp_http_client

        async def scenario():
            client = create_mcp_http_client(headers={"Authorization": f"Bearer {self.key}"})
            async with client:
                async with streamable_http_client(self.url, http_client=client) as streams:
                    async with ClientSession(streams[0], streams[1]) as session:
                        await session.initialize()
                        names = {tool.name for tool in (await session.list_tools()).tools}
                        self.assertIn("press", names)
                        result = await session.call_tool("game_status", {})
                        text = next(item.text for item in result.content if item.type == "text")
                        self.assertIn("start_game first", json.loads(text)["error"])

        asyncio.run(asyncio.wait_for(scenario(), 60))


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
