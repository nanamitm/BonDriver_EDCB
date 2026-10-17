"""python test/regression.py <build directory> [close|ready|mutex ...]

Build regression.cpp beside the DLL first. Tests use temporary DLL/INI copies
and a localhost HTTP server; an EDCB installation is not needed or contacted.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.send_ts = False
        self.stopped = threading.Event()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        try:
            if "list=1" in self.path:
                body = b'{"items":[{"onid":4,"tsid":16400,"sid":101,"serviceType":1,"serviceName":"Test"}]}'
                self.wfile.write(b"HTTP/1.0 200 OK\r\nContent-Length: " + str(len(body)).encode()
                                 + b"\r\n\r\n" + body)
                return
            # 同じsendでヘッダとTSを送り、先読みも検査する。
            body = b"\x47" + bytes(187) if self.server.send_ts else b""
            self.wfile.write(b"HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\n\r\n" + body)
            self.wfile.flush()
            self.server.stopped.wait(30)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    modes = sys.argv[2:] or ["close", "ready", "mutex"]
    server = Server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="bondriver-regression-") as temp:
            temp = pathlib.Path(temp)
            driver = temp / "BonDriver_EDCB.dll"
            second = temp / "BonDriver_EDCB_copy.dll"
            ini = ("[GLOBAL]\nSERVER_HOST=127.0.0.1\nSERVER_PORT=" + str(server.server_port)
                   + "\nSTREAM_KEY=regression\n")
            for path in (driver, second):
                shutil.copy2(build / "BonDriver_EDCB.dll", path)
                path.with_suffix(".ini").write_text(ini, encoding="ascii")
            exe = str(build / "regression.exe")

            def run(mode, *extra):
                result = subprocess.run([exe, str(driver), mode, *map(str, extra)],
                                        capture_output=True, text=True, timeout=20)
                if result.returncode:
                    raise AssertionError(f"{mode}: {result.stdout}{result.stderr}")
                print(f"{mode}: PASS", flush=True)

            for mode in modes:
                server.send_ts = mode == "ready"
                if mode != "mutex":
                    run(mode)
                    continue
                owner = subprocess.Popen([exe, str(driver), "owner"], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    # communicateのタイムアウト同様、起動待ちにも上限を付ける。
                    ready = []
                    reader = threading.Thread(target=lambda: ready.append(owner.stdout.readline()), daemon=True)
                    reader.start()
                    reader.join(10)
                    if reader.is_alive() or ready != ["READY\n"]:
                        raise AssertionError("owner did not become ready")
                    run("contender")
                    out, err = owner.communicate("\n", timeout=10)
                    if owner.returncode:
                        raise AssertionError(f"owner: {out}{err}")
                finally:
                    if owner.poll() is None:
                        owner.kill()
                        owner.communicate()
                run("duplicate", second)
                run("available")
                # Release()を呼べないプロセス異常終了でも排他が残らないこと。
                owner = subprocess.Popen([exe, str(driver), "owner"], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    ready = []
                    reader = threading.Thread(target=lambda: ready.append(owner.stdout.readline()), daemon=True)
                    reader.start()
                    reader.join(10)
                    if reader.is_alive() or ready != ["READY\n"]:
                        raise AssertionError("crash-test owner did not become ready")
                finally:
                    if owner.poll() is None:
                        owner.kill()
                    owner.communicate(timeout=10)
                run("available")
                print("mutex: PASS", flush=True)
    finally:
        server.stopped.set()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
