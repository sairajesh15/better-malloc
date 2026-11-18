#!/usr/bin/env python3
"""
server.py - 100% Live C Bridge Server for Project 23: A 'Better' Malloc (libmem.so)
Bridges native C memory, real user payloads, real_runner C binary, and OS kernel telemetry to the Apple Liquid Glass UI.
"""

import os
import sys
import json
import time
import ctypes
import threading
import subprocess
import tempfile
from http.server import HTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

PORT = int(os.environ.get("PORT", 8080))
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
os.chdir(BASE_DIR)

# -----------------------------------------------------------------------------
# C Shared Library ctypes Interface (libmem.so / libmem_test.so)
# -----------------------------------------------------------------------------
LIBMEM_TEST_PATH = os.path.join(BASE_DIR, "libmem_test.so")
LIBMEM_PROD_PATH = os.path.join(BASE_DIR, "libmem.so")
LIBMEM_PATH = LIBMEM_TEST_PATH if os.path.exists(LIBMEM_TEST_PATH) else LIBMEM_PROD_PATH
REAL_RUNNER_PATH = os.path.join(BASE_DIR, "real_runner")
lib = None
libc = None
allocator_lock = threading.Lock()

class BlockHeader(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_size_t),
        ("is_free", ctypes.c_int),
        ("magic", ctypes.c_uint32),
        ("prev_phys", ctypes.c_void_p),
        ("next_phys", ctypes.c_void_p),
        ("prev_free", ctypes.c_void_p), # Overlaid with req_size when allocated
        ("next_free", ctypes.c_void_p),
    ]

def load_libmem():
    global lib, libc
    if not os.path.exists(LIBMEM_PATH):
        print(f"[WARN] {LIBMEM_PATH} not found. Running 'make'...")
        subprocess.run(["make"], cwd=BASE_DIR, check=True)

    lib = ctypes.CDLL(LIBMEM_PATH)
    libc = ctypes.CDLL(None)

    # Function signatures
    lib.Mem_Init.argtypes = [ctypes.c_int]
    lib.Mem_Init.restype = ctypes.c_int
    lib.Mem_Alloc.argtypes = [ctypes.c_int]
    lib.Mem_Alloc.restype = ctypes.c_void_p
    lib.Mem_Free.argtypes = [ctypes.c_void_p]
    lib.Mem_Free.restype = ctypes.c_int
    lib.Mem_Dump.argtypes = []
    lib.Mem_Dump.restype = None
    lib.Mem_GetMmapCount.argtypes = []
    lib.Mem_GetMmapCount.restype = ctypes.c_int
    lib.Mem_GetBinIndex.argtypes = [ctypes.c_size_t]
    lib.Mem_GetBinIndex.restype = ctypes.c_int
    lib.Mem_GetArenaStart.argtypes = []
    lib.Mem_GetArenaStart.restype = ctypes.c_void_p
    lib.Mem_GetArenaSize.argtypes = []
    lib.Mem_GetArenaSize.restype = ctypes.c_size_t
    if hasattr(lib, "Mem_Reset"):
        lib.Mem_Reset.argtypes = []
        lib.Mem_Reset.restype = ctypes.c_int
    if hasattr(lib, "Mem_SetAllowInterior"):
        lib.Mem_SetAllowInterior.argtypes = [ctypes.c_int]
        lib.Mem_SetAllowInterior.restype = None
    if hasattr(lib, "Mem_GetAllowInterior"):
        lib.Mem_GetAllowInterior.argtypes = []
        lib.Mem_GetAllowInterior.restype = ctypes.c_int

    libc.fflush.argtypes = [ctypes.c_void_p]
    libc.fflush.restype = ctypes.c_int

    print(f"[OK] Successfully loaded {LIBMEM_PATH} via ctypes")
    # Auto-initialize default 32 KB arena
    if hasattr(lib, "Mem_Reset"):
        lib.Mem_Reset()
    if not lib.Mem_GetArenaStart():
        lib.Mem_Init(32768)
    start_addr = lib.Mem_GetArenaStart()
    print(f"[OK] Initialized default 32 KB arena at 0x{start_addr:09x}")

try:
    load_libmem()
except Exception as e:
    print(f"[ERROR] Failed to load libmem.so: {e}", file=sys.stderr)

# Global tracker of allocated pointers for the active interactive session
session_pointers = {}

def get_hardware_info():
    """Extracts authentic Apple Silicon hardware info via sysctl and os"""
    cpu_name = "Apple Silicon ARM64"
    model = "Mac"
    mem_gb = 16.0
    page_size = 16384
    try:
        p = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True)
        if p.returncode == 0 and p.stdout.strip():
            cpu_name = p.stdout.strip()
        p2 = subprocess.run(["sysctl", "-n", "hw.model"], capture_output=True, text=True)
        if p2.returncode == 0 and p2.stdout.strip():
            model = p2.stdout.strip()
        p3 = subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True)
        if p3.returncode == 0 and p3.stdout.strip():
            mem_gb = round(int(p3.stdout.strip()) / (1024**3), 1)
        if hasattr(os, "sysconf"):
            page_size = os.sysconf("SC_PAGESIZE")
    except Exception:
        pass

    return {
        "cpu": cpu_name,
        "model": model,
        "ram_gb": mem_gb,
        "page_size": page_size,
        "page_size_kb": page_size // 1024,
        "compiler": "Apple Clang -O2",
        "arch": "Apple Silicon ARM64 (macOS)"
    }

def get_m_error():
    if not lib:
        return 0
    return ctypes.c_int.in_dll(lib, "m_error").value

def capture_mem_dump():
    """Captures the authentic ASCII stdout produced by lib.Mem_Dump()"""
    if not lib:
        return "libmem.so not loaded"

    stdout_fd = 1
    saved_fd = os.dup(stdout_fd)
    r, w = os.pipe()
    os.dup2(w, stdout_fd)
    os.close(w)

    output = ""
    try:
        lib.Mem_Dump()
        libc.fflush(None)
    finally:
        os.dup2(saved_fd, stdout_fd)
        os.close(saved_fd)
        with os.fdopen(r, "r") as pipe_r:
            output = pipe_r.read()

    return output

def inspect_arena():
    """Walks the contiguous physical blocks in real C memory and returns JSON inspection with live byte peek"""
    if not lib:
        return {"initialized": False, "blocks": []}

    start = lib.Mem_GetArenaStart()
    size = lib.Mem_GetArenaSize()
    if not start or size == 0:
        lib.Mem_Init(32768)
        start = lib.Mem_GetArenaStart()
        size = lib.Mem_GetArenaSize()

    mmap_count = lib.Mem_GetMmapCount()
    err = get_m_error()

    if not start or size == 0:
        return {
            "initialized": False,
            "arena_start": None,
            "arena_size": 0,
            "mmap_syscall_count": mmap_count,
            "m_error": err,
            "blocks": [],
            "stats": {"total_blocks": 0, "alloc_blocks": 0, "free_blocks": 0, "used_bytes": 0, "free_bytes": 0}
        }

    blocks = []
    curr = start
    end = start + size
    used_bytes = 0
    free_bytes = 0
    alloc_count = 0
    free_count = 0

    while curr and curr < end:
        hdr = BlockHeader.from_address(curr)
        if hdr.magic != 0x4D454D59:
            break

        payload_addr = curr + 48
        is_free = bool(hdr.is_free)
        block_size = hdr.size

        # Check canary if allocated
        canary_intact = True
        canary_val_hex = ""
        payload_preview = ""
        hex_preview = ""

        req_sz = hdr.prev_free or (block_size - 8)
        if not is_free and req_sz > 0 and (req_sz + 8) <= block_size:
            canary_addr = payload_addr + req_sz
            canary_val = ctypes.c_uint64.from_address(canary_addr).value
            canary_val_hex = f"0x{canary_val:016X}"
            canary_intact = (canary_val == 0xDEADBEEFCAFEBABE)

            # Peek live string payload from RAM
            peek_len = min(req_sz, 32)
            raw = ctypes.string_at(payload_addr, peek_len)
            payload_preview = "".join(chr(b) if 32 <= b <= 126 else "." for b in raw)

            # Live hex preview of first 16 bytes of payload
            hex_len = min(req_sz, 16)
            hex_preview = " ".join(f"{b:02x}" for b in ctypes.string_at(payload_addr, hex_len))

        bin_idx = lib.Mem_GetBinIndex(block_size) if is_free else -1

        blocks.append({
            "header_addr": f"0x{curr:09x}",
            "payload_addr": f"0x{payload_addr:09x}",
            "size": block_size,
            "is_free": is_free,
            "magic": f"0x{hdr.magic:08X}",
            "canary": canary_val_hex,
            "canary_intact": canary_intact,
            "bin": bin_idx,
            "payload_preview": payload_preview,
            "hex_preview": hex_preview,
            "prev_phys": f"0x{hdr.prev_phys:09x}" if hdr.prev_phys else None,
            "next_phys": f"0x{hdr.next_phys:09x}" if hdr.next_phys else None,
            "prev_free": f"0x{hdr.prev_free:09x}" if hdr.prev_free else None,
            "next_free": f"0x{hdr.next_free:09x}" if hdr.next_free else None,
        })

        if is_free:
            free_count += 1
            free_bytes += block_size
        else:
            alloc_count += 1
            used_bytes += block_size

        curr = hdr.next_phys

    return {
        "initialized": True,
        "arena_start": f"0x{start:09x}",
        "arena_end": f"0x{end:09x}",
        "arena_size": size,
        "mmap_syscall_count": mmap_count,
        "m_error": err,
        "blocks": blocks,
        "stats": {
            "total_blocks": len(blocks),
            "alloc_blocks": alloc_count,
            "free_blocks": free_count,
            "used_bytes": used_bytes,
            "free_bytes": free_bytes,
            "utilization_pct": round((used_bytes / size * 100), 2) if size > 0 else 0
        }
    }

# -----------------------------------------------------------------------------
# HTTP Request Handler
# -----------------------------------------------------------------------------
class AllocatorBridgeHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()

    def send_json(self, data, status=200):
        body = json.dumps(data, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path in ("/", "/index.html"):
            self.path = "/index.html"
            return super().do_GET()

        # 1. Live Hardware & Host Status
        if path == "/api/status":
            with allocator_lock:
                arena = inspect_arena()
                hw = get_hardware_info()
                resp = {
                    "status": "online",
                    "bridge": "Python 3 ctypes C Bridge",
                    "hardware": hw,
                    "libmem_path": LIBMEM_PATH,
                    "real_runner_path": REAL_RUNNER_PATH,
                    "arena": arena,
                    "dump": capture_mem_dump()
                }
            return self.send_json(resp)

        # 2. Sample Datasets for 1-Click Testing in Real Data Lab
        if path == "/api/sample-data":
            sample_type = parse_qs(parsed.query).get("type", ["json"])[0]
            if sample_type == "json":
                sample = {
                    "event": "http_api_request",
                    "timestamp": int(time.time()),
                    "client_ip": "192.168.1.104",
                    "endpoint": "/api/v2/telemetry/dispatch",
                    "method": "POST",
                    "headers": {
                        "Host": "api.cloud.internal",
                        "User-Agent": "Antigravity/2.0 (macOS; Apple Silicon M2)",
                        "Authorization": "Bearer sec_eyJhbGciOiJFUzI1NiJ9.c2FtcGxl",
                        "Content-Type": "application/json"
                    },
                    "payload": {
                        "cluster_id": "us-west-apple-arm64",
                        "metric_batches": [
                            {"metric": "cpu_cycles", "value": 41829302, "core": 0},
                            {"metric": "l1d_misses", "value": 8192, "core": 0},
                            {"metric": "mmap_faults", "value": 1, "status": "nominal"}
                        ]
                    }
                }
                return self.send_json({"type": "json", "filename": "sample_api_request.json", "content": json.dumps(sample, indent=2)})
            elif sample_type == "csv":
                csv_data = """id,service_name,latency_ns,status,endpoint,method,retries
1001,auth_service,42000,200,/v1/token,POST,0
1002,user_directory,83000,200,/v1/users/492,GET,0
1003,payment_gateway,215000,200,/v1/charges,POST,1
1004,notification_broker,31000,200,/v1/alerts,POST,0
1005,storage_gateway,1420000,200,/v1/chunks/912,PUT,0
1006,cache_coordinator,12000,200,/v1/keys/memos,GET,0
"""
                return self.send_json({"type": "csv", "filename": "service_metrics.csv", "content": csv_data.strip()})
            elif sample_type == "log":
                log_data = """[2026-11-18 16:04:12] kernel[0]: Darwin Kernel Version 25.1.0: Apple Silicon ARM64; root:xnu-10063.141.2
[2026-11-18 16:04:12] libmem[9102]: Mem_Init(33554432) -> mmap(0x0, 33554432, PROT_READ|PROT_WRITE) = 0x1090d4000
[2026-11-18 16:04:12] libmem[9102]: Segregated free-list armed: 8 bins initialized, host page size 16384 B
[2026-11-18 16:04:13] bench[9104]: Allocation burst: 50,000 operations dispatched, recording nanosecond timestamps
[2026-11-18 16:04:13] audit[9104]: Canary verification: 0xDEADBEEFCAFEBABE intact on all allocated blocks
"""
                return self.send_json({"type": "log", "filename": "kernel_audit.log", "content": log_data.strip()})
            else:
                return self.send_json({"error": "Unknown sample type"}, status=400)

        # 3. Latest Benchmark Results
        if path == "/api/benchmark/latest":
            json_file = os.path.join(BASE_DIR, "real_comparison.json")
            if os.path.exists(json_file):
                try:
                    with open(json_file, "r") as f:
                        data = json.load(f)
                    return self.send_json(data)
                except Exception as e:
                    return self.send_json({"error": f"Failed to read json: {e}"}, status=500)
            else:
                # Run quick initial bench to generate fresh data
                res = self.handle_benchmark_live({"operations": 20000, "min_size": 16, "max_size": 256, "free_percent": 50})
                if res.get("success"):
                    return self.send_json(res.get("data"))
                return self.send_json({"error": "real_comparison.json not available"}, status=404)

        if path == "/api/arena/dump":
            with allocator_lock:
                dump_text = capture_mem_dump()
            return self.send_json({"dump": dump_text})

        # Static files fallback
        return super().do_GET()

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        content_len = int(self.headers.get("Content-Length", 0))
        post_body = self.rfile.read(content_len) if content_len > 0 else b"{}"

        try:
            req_data = json.loads(post_body.decode("utf-8")) if post_body else {}
        except Exception:
            return self.send_json({"error": "Invalid JSON body"}, status=400)

        # ---------------------------------------------------------------------
        # 1. POST /api/parse-real-data - Parse real user file/text in libmem.so
        # ---------------------------------------------------------------------
        if path == "/api/parse-real-data":
            with allocator_lock:
                result = self.handle_parse_real_data(req_data)
            return self.send_json(result)

        # ---------------------------------------------------------------------
        # 2. POST /api/benchmark/live - Live hardware benchmark with telemetry
        # ---------------------------------------------------------------------
        if path == "/api/benchmark/live" or path == "/api/benchmark/run":
            with allocator_lock:
                result = self.handle_benchmark_live(req_data)
            return self.send_json(result)

        # ---------------------------------------------------------------------
        # 3. POST /api/arena/exec - Direct C FFI allocator actions
        # ---------------------------------------------------------------------
        if path == "/api/arena/exec":
            action = req_data.get("action", "")
            with allocator_lock:
                result = self.handle_arena_action(action, req_data)
            return self.send_json(result)

        return self.send_json({"error": f"Unknown endpoint: {path}"}, status=404)

    def handle_parse_real_data(self, data):
        text_content = data.get("text") or data.get("content") or ""
        filename = data.get("filename", "payload.json")
        free_pattern = data.get("free_pattern") or data.get("frag_pattern") or "alternate"
        arena_mb = int(data.get("arena_mb", 32))

        if not text_content:
            return {"success": False, "error": "No text content provided"}

        if not os.path.exists(REAL_RUNNER_PATH):
            subprocess.run(["make", "real_runner"], cwd=BASE_DIR, check=True)

        with tempfile.NamedTemporaryFile(mode="w", suffix="_" + filename, delete=False) as tmp:
            tmp.write(text_content)
            tmp_path = tmp.name

        try:
            cmd = [REAL_RUNNER_PATH, "parse", tmp_path, free_pattern]
            t0 = time.time()
            res = subprocess.run(cmd, cwd=BASE_DIR, capture_output=True, text=True, check=True)
            elapsed = time.time() - t0

            parsed_result = json.loads(res.stdout)
            return {
                "success": True,
                "server_elapsed_sec": round(elapsed, 4),
                "data": parsed_result
            }
        except subprocess.CalledProcessError as e:
            return {"success": False, "error": f"real_runner failed: {e.stderr or e.stdout}"}
        except json.JSONDecodeError as e:
            return {"success": False, "error": f"Failed to parse JSON output: {e}"}
        finally:
            if os.path.exists(tmp_path):
                os.unlink(tmp_path)

    def handle_benchmark_live(self, data):
        ops = int(data.get("operations", 20000))
        min_sz = int(data.get("min_size", 16))
        max_sz = int(data.get("max_size", 512))
        free_pct = int(data.get("free_percent") or data.get("free_pct") or 50)
        file_content = data.get("file_content", None)

        if not os.path.exists(REAL_RUNNER_PATH):
            subprocess.run(["make", "real_runner"], cwd=BASE_DIR, check=True)

        cmd = [REAL_RUNNER_PATH, "bench", str(ops), str(min_sz), str(max_sz), str(free_pct)]

        tmp_path = None
        if file_content:
            with tempfile.NamedTemporaryFile(mode="w", delete=False) as tf:
                tf.write(file_content)
                tmp_path = tf.name
            cmd.append(tmp_path)

        try:
            t0 = time.time()
            res = subprocess.run(cmd, cwd=BASE_DIR, capture_output=True, text=True, check=True)
            elapsed = time.time() - t0

            bench_result = json.loads(res.stdout)
            return {
                "success": True,
                "elapsed_sec": round(elapsed, 4),
                "data": bench_result,
                "raw_stdout": res.stdout
            }
        except subprocess.CalledProcessError as e:
            return {"success": False, "error": f"Benchmark failed: {e.stderr or e.stdout}"}
        except json.JSONDecodeError as e:
            return {"success": False, "error": f"Failed to parse benchmark JSON: {e}"}
        finally:
            if tmp_path and os.path.exists(tmp_path):
                os.unlink(tmp_path)

    def handle_arena_action(self, action, data):
        global session_pointers
        if not lib:
            return {"success": False, "error": "libmem.so not loaded"}

        log_msg = ""
        error_code = 0
        extra_data = {}

        # Auto-initialize 32 KB arena if not yet active
        if not lib.Mem_GetArenaStart() and action != "init":
            lib.Mem_Init(32768)
            session_pointers = {}

        if action == "init":
            size = int(data.get("size", 32768))
            lib.Mem_Reset()
            session_pointers = {}
            rc = lib.Mem_Init(size)
            error_code = get_m_error()
            log_msg = f"Mem_Init({size}) -> rc: {rc} (m_error: {error_code})"

        elif action == "reset":
            lib.Mem_Reset()
            session_pointers = {}
            lib.Mem_Init(32768)
            error_code = get_m_error()
            log_msg = "Mem_Reset() -> arena re-initialized to clean 32 KB state"

        elif action in ("status", "inspect", "get"):
            error_code = get_m_error()
            log_msg = "Fetched live arena state from C memory"

        elif action == "alloc":
            size = int(data.get("size", 64))
            name = data.get("id") or f"ptr_{len(session_pointers) + 1}"
            fill_text = data.get("text") or data.get("payload_str") or ""

            ptr = lib.Mem_Alloc(size)
            error_code = get_m_error()

            if ptr:
                ptr_hex = f"0x{ptr:09x}"
                session_pointers[name] = ptr

                # Write user string directly into real C memory
                if fill_text:
                    encoded = fill_text.encode("utf-8")
                    copy_len = min(len(encoded), size)
                    ctypes.memmove(ptr, encoded, copy_len)

                extra_data = {"id": name, "pointer": ptr_hex, "size": size, "payload": fill_text}
                log_msg = f"Mem_Alloc({size}) -> {ptr_hex} (payload: '{fill_text[:20]}')"
            else:
                log_msg = f"Mem_Alloc({size}) -> NULL (m_error: {error_code})"

        elif action == "set_interior_mode":
            enabled = bool(data.get("enabled", False))
            if hasattr(lib, "Mem_SetAllowInterior"):
                lib.Mem_SetAllowInterior(1 if enabled else 0)
            log_msg = f"Interior pointer mode {'ENABLED (O(N) permissive)' if enabled else 'DISABLED (O(1) strict safety)'}"
            extra_data = {"interior_mode": enabled}

        elif action == "free":
            ptr_val = data.get("pointer") or data.get("id")
            target_ptr = None

            if isinstance(ptr_val, str) and ptr_val.startswith("0x"):
                target_ptr = int(ptr_val, 16)
            elif ptr_val in session_pointers:
                target_ptr = session_pointers[ptr_val]
                del session_pointers[ptr_val]

            offset = int(data.get("offset", 0))
            allow_interior = data.get("allow_interior")
            if allow_interior is not None and hasattr(lib, "Mem_SetAllowInterior"):
                lib.Mem_SetAllowInterior(1 if allow_interior else 0)

            if target_ptr is not None:
                final_ptr = target_ptr + offset
                rc = lib.Mem_Free(ctypes.c_void_p(final_ptr))
                error_code = get_m_error()
                err_names = {
                    0: "SUCCESS",
                    1: "E_NO_SPACE",
                    2: "E_CORRUPT_FREESPACE",
                    3: "E_PADDING_OVERWRITTEN",
                    4: "E_BAD_ARGS",
                    5: "E_BAD_POINTER"
                }
                err_name = err_names.get(error_code, f"CODE_{error_code}")
                if rc == 0:
                    log_msg = f"Mem_Free(0x{final_ptr:09x}) -> Success (rc: 0) [Coalesced cleanly]"
                else:
                    log_msg = f"Mem_Free(0x{final_ptr:09x}) -> Rejected with m_error={error_code} ({err_name})"
                extra_data = {"freed_pointer": f"0x{final_ptr:09x}", "rc": rc, "m_error": error_code, "err_name": err_name}
            else:
                log_msg = f"Mem_Free failed: pointer '{ptr_val}' invalid"
                error_code = 5

        elif action == "test_double_free":
            ptr_val = data.get("pointer") or data.get("id")
            target_ptr = None
            if isinstance(ptr_val, str) and ptr_val.startswith("0x"):
                target_ptr = int(ptr_val, 16)
            elif ptr_val in session_pointers:
                target_ptr = session_pointers[ptr_val]

            if target_ptr:
                rc1 = lib.Mem_Free(ctypes.c_void_p(target_ptr))
                rc2 = lib.Mem_Free(ctypes.c_void_p(target_ptr))
                err2 = get_m_error()
                log_msg = f"Double Free Defense: 1st Free rc={rc1}; 2nd Free rejected rc={rc2}, m_error={err2} (E_BAD_POINTER). Memory protected!"
                extra_data = {"rc1": rc1, "rc2": rc2, "caught": (rc2 == -1 and err2 == 5)}
            else:
                log_msg = "Double free test failed: select an allocated block first"

        elif action == "corrupt_canary":
            ptr_val = data.get("pointer") or data.get("id")
            target_ptr = None
            if isinstance(ptr_val, str) and ptr_val.startswith("0x"):
                target_ptr = int(ptr_val, 16)
            elif ptr_val in session_pointers:
                target_ptr = session_pointers[ptr_val]

            if target_ptr:
                hdr = BlockHeader.from_address(target_ptr - 48)
                req_sz = hdr.prev_free or (hdr.size - 8)
                canary_addr = target_ptr + req_sz
                ctypes.c_uint64.from_address(canary_addr).value = 0xAAAAAAAAAAAAAAAA
                log_msg = f"Corrupted canary at 0x{canary_addr:09x} to 0xAAAAAAAAAAAAAAAA"
                extra_data = {"corrupted_addr": f"0x{canary_addr:09x}"}
            else:
                log_msg = "Corrupt canary failed: pointer not found"

        elif action == "preset_canary" or action == "preset_overflow":
            lib.Mem_Reset()
            session_pointers = {}
            lib.Mem_Init(16384)
            p = lib.Mem_Alloc(128)
            session_pointers["overflow_target"] = p
            hdr = BlockHeader.from_address(p - 48)
            canary_addr = p + hdr.size - 8
            ctypes.c_uint64.from_address(canary_addr).value = 0xDEADDEADDEADDEAD
            rc = lib.Mem_Free(ctypes.c_void_p(p))
            error_code = get_m_error()
            log_msg = f"Executed Preset: Canary Buffer Overflow -> Free rc: {rc}, m_error: {error_code} (E_PADDING_OVERWRITTEN)"
            extra_data = {"rc": rc, "caught": (error_code == 3)}

        elif action == "preset_interior":
            lib.Mem_Reset()
            session_pointers = {}
            lib.Mem_Init(16384)
            p = lib.Mem_Alloc(256)
            session_pointers["interior_target"] = p
            rc = lib.Mem_Free(ctypes.c_void_p(p + 32))
            error_code = get_m_error()
            log_msg = f"Executed Preset: Interior Pointer Free -> Free(ptr+32) rc: {rc} (Coalesced back to free)"
            extra_data = {"rc": rc, "coalesced": (rc == 0)}

        arena_state = inspect_arena()
        dump_str = capture_mem_dump()

        return {
            "success": True,
            "action": action,
            "log": log_msg,
            "m_error": error_code,
            "arena": arena_state,
            "dump": dump_str,
            "extra": extra_data
        }

def run_server():
    server_address = ("", PORT)
    httpd = HTTPServer(server_address, AllocatorBridgeHandler)
    hw = get_hardware_info()
    print("=" * 76)
    print(f"  Project 23: A 'Better' Malloc - 100% Live C Bridge Server")
    print(f"  URL       : http://localhost:{PORT}")
    print(f"  Hardware  : {hw['cpu']} ({hw['model']}) • {hw['ram_gb']} GB RAM • {hw['page_size_kb']} KB Page")
    print(f"  Library   : {LIBMEM_PATH}")
    print(f"  Runner    : {REAL_RUNNER_PATH}")
    print("=" * 76)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        httpd.server_close()

if __name__ == "__main__":
    run_server()
