"""验证主程序配置入口，以及非默认 TCP/组播端点上的订单闭环。"""
import argparse
import collections
import json
import os
import pathlib
import select
import signal
import socket
import struct
import subprocess
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def cli_contract(build, configs, evidence):
    client = str(build / "trading_main")
    exchange = str(build / "exchange_main")
    path = str(configs / "client.json")
    cases = [
        ([exchange, "--check-config"], 0, {}),
        ([client, "--check-config"], 0, {}),
        ([client, "--config", path, "--check-config", "12", "TAKER",
          "2", "0.5", "8", "20", "-10"], 0, {}),
        ([exchange, "--config", ""], 2, {}),
        ([client, "--config", ""], 2, {}),
        ([client, "--config"], 2, {}),
        ([client, "--config", path, "--config", path], 2, {}),
        ([client, "--config", str(evidence / "missing.json")], 2, {}),
        ([client, "--check-config", "-1", "MAKER"], 2, {}),
        ([client, "--check-config", "12x", "MAKER"], 2, {}),
        ([client, "--check-config", "12", "UNKNOWN"], 2, {}),
        ([client, "--check-config", "12", "MAKER", "2", "nan", "8", "20", "-10"], 2, {}),
        ([client, "--check-config", "12", "MAKER", "2", "1", "8x", "20", "-10"], 2, {}),
        ([client, "--check-config"], 2, {"TRADE_RUN_SECONDS": "-1"}),
        ([client, "--check-config"], 2, {"TRADE_RANDOM_ORDERS": "2junk"}),
        ([client, "--check-config"], 2, {"TRADE_RANDOM_DELAY_US": "1000001"}),
    ]
    results = []
    for index, (command, code, overrides) in enumerate(cases):
        directory = evidence / f"cli-{index:02}"
        directory.mkdir()
        env = dict(os.environ)
        for key in ("TRADE_RUN_SECONDS", "TRADE_RANDOM_ORDERS", "TRADE_RANDOM_DELAY_US"):
            env.pop(key, None)
        env.update(overrides)
        result = subprocess.run(command, cwd=directory, env=env,
                                capture_output=True, text=True, timeout=15)
        (directory / "output.txt").write_text(result.stdout + result.stderr)
        require(result.returncode == code, f"CLI case {index} exit {result.returncode}, expected {code}")
        require(not list(directory.glob("*.journal")) and not list(directory.glob("*.log")),
                f"CLI case {index} started runtime resources")
        if index == 2:
            require("client:12 strategy:TAKER" in result.stdout, "legacy override not applied")
        results.append({"case": index, "exit": result.returncode})
    return results


def subscribe(group, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", port))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                   socket.inet_aton(group) + socket.inet_aton("127.0.0.1"))
    sock.setblocking(False)
    return sock


def network_contract(build, configs, evidence):
    exchange_config = json.loads((configs / "exchange.json").read_text())
    client_config = json.loads((configs / "client.json").read_text())
    market = {"interface": "lo", "snapshot": {"address": "233.252.15.11", "port": 29011},
              "incremental": {"address": "233.252.15.13", "port": 29013}}
    tcp = {"address": "127.0.0.2", "port": 29015}
    exchange_config.update(market_data=market, tcp=tcp)
    client_config.update(client_id=7, market_data=market, tcp=tcp,
                         runtime={"run_seconds": 4, "random_orders": 12, "random_delay_us": 30000})
    client_config["strategy"]["type"] = "RANDOM"
    client_config["strategy"]["default_parameters"]["risk"].update(
        max_order_size=1000, max_position=100000, max_loss=-1000000000)
    paths = []
    for name, value in (("exchange", exchange_config), ("client", client_config)):
        path = evidence / f"custom-{name}.json"
        path.write_text(json.dumps(value, indent=2))
        paths.append(path)
    exchange_dir, client_dir = evidence / "exchange-run", evidence / "client-run"
    exchange_dir.mkdir()
    client_dir.mkdir()
    snapshot = subscribe(market["snapshot"]["address"], market["snapshot"]["port"])
    incremental = subscribe(market["incremental"]["address"], market["incremental"]["port"])
    processes = []
    outputs = []
    events = {"snapshot": [], "incremental": []}
    try:
        for directory in (exchange_dir, client_dir):
            outputs.append((directory / "process-output.txt").open("w"))
        env = dict(os.environ)
        env["TRADE_SNAPSHOT_MS"] = "200"
        for key in ("TRADE_RUN_SECONDS", "TRADE_RANDOM_ORDERS", "TRADE_RANDOM_DELAY_US"):
            env.pop(key, None)
        exchange = subprocess.Popen([str(build / "exchange_main"), "--config", str(paths[0])],
                                    cwd=exchange_dir, env=env, stdout=outputs[0], stderr=subprocess.STDOUT)
        processes.append(exchange)
        deadline = time.monotonic() + 8
        while True:
            require(exchange.poll() is None, "exchange exited before TCP listening")
            try:
                with socket.create_connection((tcp["address"], tcp["port"]), timeout=.1):
                    break
            except OSError:
                require(time.monotonic() < deadline, "configured TCP listener unavailable")
                time.sleep(.02)
        try:
            with socket.create_connection(("127.0.0.1", tcp["port"]), timeout=.2):
                raise RuntimeError("listener ignored configured 127.0.0.2 bind address")
        except OSError:
            pass
        client = subprocess.Popen([str(build / "trading_main"), "--config", str(paths[1])],
                                  cwd=client_dir, env=env, stdout=outputs[1], stderr=subprocess.STDOUT)
        processes.append(client)
        deadline = time.monotonic() + 20
        while client.poll() is None:
            require(time.monotonic() < deadline, "client shutdown timed out")
            readable, _, _ = select.select([snapshot, incremental], [], [], .05)
            for sock in readable:
                data = sock.recv(65535)
                require(len(data) % 70 == 0, "invalid market datagram length")
                name = "snapshot" if sock is snapshot else "incremental"
                for offset in range(0, len(data), 70):
                    frame = data[offset:offset + 70]
                    require(frame[:4] == b"TS\x01\x03", "wrong market wire header")
                    sequence, cycle, watermark, declared_state = struct.unpack_from(">QQQQ", frame, 4)
                    events[name].append({"sequence": sequence, "cycle": cycle, "watermark": watermark,
                                         "declared_state": declared_state, "type": frame[36]})
        require(client.returncode == 0, "client session/shutdown failed")
        exchange.send_signal(signal.SIGTERM)
        require(exchange.wait(timeout=15) == 0, "exchange shutdown failed")
        require(events["incremental"], "configured incremental multicast has no business events")
        require(all(e["cycle"] == e["watermark"] == e["declared_state"] == 0 and 1 <= e["type"] <= 5
                    for e in events["incremental"]), "snapshot traffic on incremental channel")
        require(any(e["type"] == 2 for e in events["incremental"]), "no ADD event")
        require(any(e["type"] == 4 for e in events["incremental"]), "no CANCEL event")
        require(any(e["type"] == 7 and e["watermark"] > 0 for e in events["snapshot"]),
                "configured snapshot multicast has no complete later snapshot")
        journal = (exchange_dir / "exchange_orders.journal").read_text().splitlines()
        received = [x.split()[1:] for x in journal if x.startswith("RECEIVED ")]
        applied = [x.split()[1:] for x in journal if x.startswith("APPLY ")]
        require(received and received == applied, "received/applied request journal differs")
        new_orders = [x for x in applied if x[3] == "1"]
        require(len(new_orders) == 12, f"expected 12 configured random orders, got {len(new_orders)}")
        require(all(x[0] == "7" for x in received), "client ID did not come from JSON")
        engine_log = (client_dir / "trading_engine_7.log").read_text()
        consumer_log = (client_dir / "trading_market_data_consumer_7.log").read_text()
        account = (client_dir / "trading_account_7.journal").read_text()
        require("RECOVERY APPLIED" in engine_log and "RECOVERY PUBLISHED" in consumer_log,
                "client did not apply full recovery before trading")
        require("UNRESOLVED" not in account, "shutdown left unresolved reservations")
        exchange_log = (exchange_dir / "exchange_main.log").read_text()
        client_log = (client_dir / "trading_main_7.log").read_text()
        require("request_queue:0 response_queue:0 md_queue:0" in exchange_log, "exchange queues not drained")
        require("request_queue:0 response_queue:0 md_queue:0 session_healthy:1" in client_log,
                "client queues/session not clean on shutdown")
        return {"new_orders": len(new_orders), "received_applied": len(applied),
                "incremental_events": len(events["incremental"]),
                "snapshot_frames": len(events["snapshot"]), "tcp": tcp, "market_data": market}
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for sock in (snapshot, incremental):
            sock.close()
        for output in outputs:
            output.close()
        (evidence / "captured-market-events.json").write_text(json.dumps(events, indent=2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("configs", type=pathlib.Path)
    parser.add_argument("evidence", type=pathlib.Path)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    results = {"cli": cli_contract(args.build.resolve(), args.configs.resolve(), args.evidence.resolve())}
    results["network"] = network_contract(args.build.resolve(), args.configs.resolve(), args.evidence.resolve())
    (args.evidence / "results.json").write_text(json.dumps(results, indent=2))
    print("STARTUP_INTEGRATION_PASS", json.dumps(results["network"]))


if __name__ == "__main__":
    main()
