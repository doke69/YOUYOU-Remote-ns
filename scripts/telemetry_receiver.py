#!/usr/bin/env python3
"""Private-LAN receiver for YOUYOU remote schema-12 four-part diagnostics.

Stdlib only. Reads a private JSON config, binds an explicitly configured
private/loopback IPv4 address, validates the pair and strict allowlisted
fields, merges exactly four datagrams, and writes sanitized JSONL/latest.

No responses are sent. Pair values are never printed or persisted.
"""
import argparse, hmac, ipaddress, json, math, os, re, socket, sys, time
from collections import defaultdict
from pathlib import Path

PARTS = 4
KINDS = {"core", "network", "media", "render"}
COMMON = {"schema", "parts", "kind", "pair", "session", "seq", "epoch", "ticks_ms", "streaming"}
FIELDS = {
    "core": {"cpu_net","cpu_dec","cpu_ui","cpu_ctl","mask_net","mask_dec","mask_ui","mask_ctl","cpu_net_rc","cpu_dec_rc","cpu_ui_rc","cpu_ctl_rc","version","width","height","requested_fps","codecflag","actualmbps","targetcap","feedback",
             "vsync","buffer","pacing","snapshot_age_ms","session_age_ms","sent","dropdiag","path",
             "local","remote","send_us","recv_us","publish_us",
             "udp_match","udp_rx_bytes","udp_tx_bytes","udp_rx_errno","udp_tx_errno"},
    "network": {"udp_calls","udp_msgs","udp_multi","udp_batch_max","udp_fallback","udp_batch_rc","rtt_ms","jitter_ms","initial_gap_pct","packets","lost","accepted","rx_drop","wait_idr",
                "idr_ff","wait_recent","wait_max","pending","block_ms","repair_ms","nack","rtx","pli",
                "nack_retry","nack_repeat","kf_requests",
                "rx_buf","rx_buf_high","rx_msgs","rx_batches","rx_max_batch","rx_in_calls",
                "rx_tick_calls","rx_in_us","rx_in_max_us","rx_tick_us","rx_tick_max_us",
                "rx_mem_drop","rx_frame_max","repair_paced","nack_seq","nack_defer","nack_old"},
    "media": {"decoded","published","cpu_xfer","gpu_pub","dec_fps","dec_ema","dec_err","drops","q_in",
              "ready","skip_due","skip_bound","late","hw","au_max","au_bytes","au_count","au_over",
              "q_high_frames","q_high_bytes","q_wait_last","q_wait_max","q_wait_oldest","dec_max",
              "dec_gt8","dec_gt16","dec_gt33","last_dec_age"},
    "render": {"display_fps","render_hz","upload_avg_ms","upload_max_ms","work_avg_ms","work_max_ms",
               "present_avg_ms","present_max_ms","loop_avg_ms","loop_max_ms","loop_over25","loop_over50",
               "gap_p95_ms","gap_max_ms","gap_overflow","frame_age_ms","present_total","native_display",
               "gpu_present","map_hit","map_new","map_live","active_ui","stats_flag","refresh_ms","stable_swaps"},
}
DISPATCH_FIELDS = {"md_enabled","md_active","md_accepted","md_done","md_q","md_bytes","md_high_bytes","md_overflow","md_fail","md_wait_max_us"}
FIELDS["render"] |= DISPATCH_FIELDS
STRINGS = {"version", "path", "local", "remote"}
FLAGS = {"streaming","codecflag","feedback","vsync","pacing","hw","native_display","active_ui","stats_flag","udp_match","repair_paced"}
SESSION_RE = re.compile(r"^[A-Za-z0-9_-]{1,48}$")
EXPECTED_VERSIONS = {"0.1.58", "0.1.59", "0.1.60"}
REPAIR_FIELDS = {"repair_paced","nack_seq","nack_defer","nack_old"}
BATCH_FIELDS = {'udp_multi', 'udp_msgs', 'udp_calls', 'udp_batch_rc', 'udp_batch_max', 'udp_fallback'}
CPU_FIELDS = {'cpu_net_rc', 'cpu_ui', 'cpu_ctl_rc', 'mask_net', 'cpu_dec_rc', 'cpu_ui_rc', 'cpu_dec', 'mask_ui', 'cpu_ctl', 'cpu_net', 'mask_dec', 'mask_ctl'}
SOCKET_FIELDS = {"udp_match","udp_rx_bytes","udp_tx_bytes","udp_rx_errno","udp_tx_errno"}


def load_config(path):
    try:
        cfg = json.loads(Path(path).read_text(encoding="utf-8"))
    except Exception:
        raise SystemExit("invalid config file")
    if not isinstance(cfg, dict):
        raise SystemExit("invalid config file")
    pair = cfg.get("pair")
    if not isinstance(pair, str) or len(pair) != 48 or any(c not in "0123456789abcdef" for c in pair):
        raise SystemExit("config pair must be 48 lowercase hex characters")
    host = cfg.get("host")
    bind = cfg.get("bind")
    port = cfg.get("port")
    if not isinstance(host, str) or not isinstance(bind, str):
        raise SystemExit("config host/bind must be strings")
    if not isinstance(port, int) or isinstance(port, bool) or not (1024 <= port <= 65535):
        raise SystemExit("config port must be 1024..65535")
    try:
        host_ip = ipaddress.IPv4Address(host)
        bind_ip = ipaddress.IPv4Address(bind)
    except ValueError:
        raise SystemExit("host/bind must be IPv4 addresses")
    if not (host_ip.is_private or host_ip.is_loopback):
        raise SystemExit("host must be private/loopback IPv4")
    if not (bind_ip.is_private or bind_ip.is_loopback):
        raise SystemExit("bind must be private/loopback IPv4")
    return pair, host_ip, bind_ip, port


def finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def positive_int(value, maximum=(1 << 63) - 1):
    return isinstance(value, int) and not isinstance(value, bool) and 0 < value <= maximum


def nonnegative_int(value, maximum=(1 << 63) - 1):
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= maximum


def validate(obj, pair):
    if not isinstance(obj, dict) or obj.get("schema") not in (12, 13, 14, 15, 16, 17) or obj.get("parts") != PARTS:
        return "schema"
    kind = obj.get("kind")
    if not isinstance(kind, str) or kind not in KINDS:
        return "kind"
    expected = FIELDS[kind]
    if obj["schema"] < 17: expected = expected - DISPATCH_FIELDS
    if obj["schema"] < 16: expected = expected - BATCH_FIELDS
    if obj["schema"] < 15: expected = expected - CPU_FIELDS
    if obj["schema"] < 14: expected = expected - REPAIR_FIELDS
    if obj["schema"] == 12: expected = expected - SOCKET_FIELDS
    if set(obj.keys()) != COMMON | expected:
        return "keys"
    pair_value = obj.get("pair")
    if not isinstance(pair_value, str) or len(pair_value) != 48 or any(c not in "0123456789abcdef" for c in pair_value):
        return "pair_type"
    if not hmac.compare_digest(pair_value, pair):
        return "auth"
    session = obj.get("session")
    if not isinstance(session, str) or not SESSION_RE.fullmatch(session):
        return "session"
    if not positive_int(obj.get("seq")):
        return "seq_type"
    if not nonnegative_int(obj.get("epoch")) or not nonnegative_int(obj.get("ticks_ms")):
        return "common_type"
    if obj.get("streaming") not in (0, 1):
        return "streaming_type"
    if kind == "core":
        versions = {"0.1.69", "0.1.70", "0.1.71", "0.1.72", "0.1.73", "0.1.74"} if obj["schema"] == 16 else {"0.1.68"} if obj["schema"] == 15 else EXPECTED_VERSIONS if obj["schema"] == 12 else ({"0.1.65", "0.1.66", "0.1.67"} if obj["schema"] == 14 else {"0.1.61", "0.1.62", "0.1.63", "0.1.64"})
        if obj["schema"] == 17: versions={"0.1.75","0.1.76","0.1.77","0.1.78","0.2.0"}
        if obj.get("version") not in versions:
            return "version"
    for key, value in obj.items():
        if key in COMMON:
            continue
        if key in STRINGS:
            if not isinstance(value, str) or len(value) > 96:
                return "string_type"
            continue
        if not finite_number(value):
            return "number_type"
        if key in FLAGS and value not in (0, 1):
            return "flag_type"
    return None


COMMON_SAMPLE = ("schema", "parts", "pair", "session", "seq", "epoch", "ticks_ms", "streaming")


def common_consistent(parts):
    if not parts:
        return False
    first = next(iter(parts.values()))
    for part in parts.values():
        for key in COMMON_SAMPLE:
            if part.get(key) != first.get(key):
                return False
    return True


class Receiver:
    def __init__(self, out: Path):
        self.out = out
        self.out.mkdir(parents=True, exist_ok=True)
        self.log = out / "telemetry.jsonl"
        self.latest = out / "latest.json"
        self.pending = {}
        self.last_seq = {}
        self.max_seq = {}
        self.expected_seq = {}
        self.health_path = out / "health.json"
        self.stats = defaultdict(int)
        self.last_summary = 0.0
        self.last_packet = 0.0
        self.started = time.time()
        self.last_sample = None
        self.session_seen = {}
        self.bound = ""
        self.port = 0

    def touch_session(self, session, epoch, now):
        tracking = (session, epoch)
        self.session_seen[tracking] = now
        if len(self.session_seen) > 64:
            oldest = min(self.session_seen, key=self.session_seen.get)
            self.session_seen.pop(oldest, None)
            for table in (self.last_seq, self.max_seq, self.expected_seq):
                table.pop(oldest, None)
            self.stats["session_tracking_evicted"] += 1

    def append_record(self, data):
        self.rotate()
        try:
            with self.log.open("a", encoding="utf-8") as out:
                out.write(json.dumps(data, ensure_ascii=False, sort_keys=True) + "\n")
            return True
        except OSError:
            self.stats["log_errors"] += 1
            return False

    def write_json_atomic(self, path, data):
        tmp = path.with_suffix(path.suffix + ".tmp")
        for _ in range(12):
            try:
                tmp.write_text(json.dumps(data, ensure_ascii=False, sort_keys=True), encoding="utf-8")
                os.replace(tmp, path)
                return True
            except (PermissionError, OSError):
                time.sleep(0.01)
        self.stats["write_errors"] += 1
        return False

    def write_health(self, now, state):
        rejected = sum(v for k, v in self.stats.items() if k.startswith("rejected_") or k in ("invalid_json", "duplicate_kind", "expired"))
        self.write_json_atomic(self.health_path, {
            "pid": os.getpid(), "bound": self.bound, "port": self.port,
            "state": state, "waiting": len(self.pending),
            "receiving": self.last_packet > 0 and now - self.last_packet <= 3.0,
            "last_age_s": round(now - self.last_packet, 3) if self.last_packet else -1,
            "complete": self.stats["complete"], "partial": self.stats["partial"],
            "rejected": rejected, "expired": self.stats["expired"],
            "log_errors": self.stats["log_errors"],
            "receiver_sequence_gaps": self.stats["receiver_sequence_gaps"],
        })

    def write_partial(self, key, now):
        entry = self.pending.pop(key, None)
        if not entry:
            return
        first, parts = entry
        if not parts:
            return
        envelope = parts.get("core") or next(iter(parts.values()))
        merged = {k: v for k, v in envelope.items() if k not in ("kind", "pair")}
        for part in parts.values():
            for name, value in part.items():
                if name in COMMON:
                    continue
                merged[name] = value
        merged["received_at"] = int(now * 1000)
        merged["complete"] = False
        merged["parts_received"] = len(parts)
        merged["missing_kinds"] = sorted(KINDS - set(parts))
        if self.append_record(merged):
            self.stats["partial"] += 1

    def rotate(self):
        try:
            if self.log.exists() and self.log.stat().st_size > 20 * 1024 * 1024:
                for i in (2, 1):
                    src = self.out / f"telemetry.{i}.jsonl"
                    dst = self.out / f"telemetry.{i+1}.jsonl"
                    if src.exists():
                        os.replace(src, dst)
                os.replace(self.log, self.out / "telemetry.1.jsonl")
        except OSError:
            self.stats["rotate_errors"] += 1

    def write_latest(self, data):
        tmp = self.latest.with_suffix(".tmp")
        for _ in range(12):
            try:
                tmp.write_text(json.dumps(data, ensure_ascii=False, sort_keys=True), encoding="utf-8")
                os.replace(tmp, self.latest)
                return
            except (PermissionError, OSError):
                time.sleep(0.01)
        self.stats["latest_errors"] += 1

    def accept(self, raw, source_ip, now):
        try:
            obj = json.loads(raw.decode("utf-8"))
        except Exception:
            self.stats["invalid_json"] += 1
            return
        kind = obj.get("kind") if isinstance(obj, dict) else None
        err = validate(obj, self.pair)
        if err:
            self.stats["rejected_" + err] += 1
            return
        if source_ip != self.host:
            self.stats["rejected_source"] += 1
            return
        self.last_packet = now
        session = obj["session"]
        epoch = int(obj["epoch"])
        self.touch_session(session, epoch, now)
        seq = int(obj["seq"])
        key = (session, epoch, seq)
        if seq <= self.last_seq.get((session, epoch), -1):
            self.stats["rejected_stale"] += 1
            return
        if seq < self.max_seq.get((session, epoch), -1):
            self.stats["rejected_reorder"] += 1
            return
        if key not in self.pending:
            if len(self.pending) >= 64:
                oldest = min(self.pending, key=lambda k: self.pending[k][0])
                self.write_partial(oldest, now)
                self.stats["pending_evicted"] += 1
            expected = self.expected_seq.get((session, epoch), seq)
            if seq > expected:
                self.stats["receiver_sequence_gaps"] += seq - expected
            self.expected_seq[(session, epoch)] = max(expected, seq + 1)
            self.max_seq[(session, epoch)] = max(self.max_seq.get((session, epoch), seq), seq)
            self.pending[key] = (now, {})
        first, parts = self.pending[key]
        if now - first > 5.0:
            self.write_partial(key, now)
            self.stats["expired"] += 1
            return
        if kind in parts:
            self.stats["duplicate_kind"] += 1
            return
        parts[kind] = obj
        if len(parts) != PARTS:
            return
        if not common_consistent(parts):
            self.pending.pop(key, None)
            self.stats["rejected_common"] += 1
            return
        base = {k: v for k, v in parts["core"].items() if k not in ("kind", "pair")}
        merged = dict(base)
        for k in KINDS:
            part = parts[k]
            for name, value in part.items():
                if name in COMMON:
                    continue
                merged[name] = value
        merged["received_at"] = int(time.time()*1000)
        merged["complete"] = True
        merged["parts_received"] = PARTS
        merged["receiver_sequence_gaps"] = self.stats["receiver_sequence_gaps"]
        self.pending.pop(key, None)
        self.last_seq[(session, epoch)] = seq
        self.last_sample = merged
        self.stats["complete"] += 1
        self.append_record(merged)
        self.write_latest(merged)
        now_s = time.time()
        if now_s - self.last_summary >= 1.0:
            self.last_summary = now_s
            print("live fps=%.1f mbps=%.1f rtt=%s decode_ms=%.1f qgap=%s complete=%d" % (
                float(merged.get("display_fps", 0)), float(merged.get("actualmbps", 0)),
                merged.get("rtt_ms"), float(merged.get("dec_max", 0)), merged.get("q_wait_oldest", 0),
                self.stats["complete"]), flush=True)

    def expire(self, now):
        for key in [k for k, v in self.pending.items() if now - v[0] > 5.0]:
            self.write_partial(key, now)
            self.stats["expired"] += 1
        for tracking in [k for k, seen in self.session_seen.items() if now - seen > 60.0]:
            self.session_seen.pop(tracking, None)
            for table in (self.last_seq, self.max_seq, self.expected_seq):
                table.pop(tracking, None)


def main():
    ap = argparse.ArgumentParser(description="Private schema-12/13/14/15/16 telemetry receiver (no replies)")
    ap.add_argument("--config", required=True, help="private JSON: pair, host=expected Switch IP, bind=PC IP, port")
    ap.add_argument("--out", required=True, help="output directory")
    args = ap.parse_args()
    pair, host, bind, port = load_config(args.config)
    rx = Receiver(Path(args.out)); rx.pair = pair; rx.host = host
    rx.bound = str(bind); rx.port = port
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((str(bind), port))
    sock.settimeout(1.0)
    rx.write_health(time.time(), "waiting")
    print("receiver bound; waiting for schema12/13/14/15/16/17 parts", flush=True)
    last_health = 0.0
    try:
        while True:
            try:
                raw, addr = sock.recvfrom(1201)
            except socket.timeout:
                now = time.time(); rx.expire(now)
                if now - last_health >= 1.0:
                    rx.write_health(now, "waiting"); last_health = now
                continue
            if len(raw) > 1200:
                rx.stats["oversize_datagram"] += 1; continue
            now = time.time()
            rx.accept(raw, ipaddress.IPv4Address(addr[0]), now)
            rx.expire(now)
            if now - last_health >= 1.0:
                rx.write_health(now, "receiving" if rx.last_packet and now - rx.last_packet <= 3.0 else "idle")
                last_health = now
    except KeyboardInterrupt:
        pass
    finally:
        rx.stats["partial_pending"] = len(rx.pending)
        rx.write_health(time.time(), "stopped")
        print("receiver stopped: " + json.dumps(dict(sorted(rx.stats.items())), sort_keys=True), flush=True)
        sock.close()

if __name__ == "__main__":
    main()
