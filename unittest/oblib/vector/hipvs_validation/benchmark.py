"""Live SQL smoke/measurement driver, NOT a claim of competition acceptance.

Each run imports data without an index, times CREATE VECTOR INDEX + a first real
query, checks every query and saves its plan. Index-internal completeness and GPU
kernel/fallback evidence still require a version-specific audit on the server.
"""
import argparse
import hashlib
import json
import math
import os
import statistics
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from metrics import score_results, vector_literal, write_json


def load_dataset(path):
    path = Path(path)
    manifest = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
    for name in ("vectors.npz", "truth.json"):
        actual = hashlib.sha256((path / name).read_bytes()).hexdigest()
        if manifest["files"][name] != actual:
            raise ValueError(f"dataset hash mismatch: {name}")
    with np.load(path / "vectors.npz", allow_pickle=False) as data:
        base, queries, ids = data["base"], data["queries"], data["ids"]
    if base.dtype != np.float32 or queries.dtype != np.float32 or ids.dtype != np.int64:
        raise ValueError("unexpected dataset dtype")
    if base.shape != (manifest["n"], manifest["dimensions"]) or queries.shape != (manifest["queries"], manifest["dimensions"]):
        raise ValueError("dataset shape mismatch")
    if len(ids) != len(base) or len(np.unique(ids)) != len(ids) or not np.isfinite(base).all() or not np.isfinite(queries).all():
        raise ValueError("invalid vector or ID data")
    if manifest.get("tie_at_k_queries"):
        raise ValueError("ties at cutoff require an explicit tie-aware competition rule")
    truth = json.loads((path / "truth.json").read_text(encoding="utf-8"))
    return manifest, base, queries, ids, truth


def fetch(cur, sql, args=None):
    cur.execute(sql, args)
    return cur.fetchall()


def query_sql(table, k):
    return f"SELECT id,l2_distance(v,%s) AS distance FROM `{table}` ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT {k}"


def neighbor_record(nid, distance):
    distance = float(distance)
    # Preserve invalid server values as text so strict JSON can still save the failure evidence.
    return {"id": int(nid), "distance": distance if math.isfinite(distance) else str(distance)}


def run_round(conn, backend, iteration, config, base, queries, ids, truth, out):
    table = f"{backend}_{iteration:02d}_{uuid.uuid4().hex[:8]}"
    lib = {"cpu": "vsag", "gpu": "cuvs"}[backend]
    record = {"backend_requested": backend, "library_requested": lib, "table": table, "iteration": iteration,
              "utc": datetime.now(timezone.utc).isoformat(), "status": "running",
              "gpu_execution_verified": False, "build_completeness_verified": False,
              "eligible_for_competition_speedup": False}
    destination = out / f"{table}.json"
    write_json(destination, record)
    try:
        with conn.cursor() as cur:
            cur.execute(f"CREATE TABLE `{table}` (id BIGINT PRIMARY KEY, v VECTOR({base.shape[1]}))")
            start = time.perf_counter()
            for first in range(0, len(base), 200):
                values = [(int(ids[i]), vector_literal(base[i])) for i in range(first, min(first + 200, len(base)))]
                cur.executemany(f"INSERT INTO `{table}`(id,v) VALUES(%s,%s)", values)
            record["import_seconds"] = time.perf_counter() - start
            # VECTOR's display formatter can round coordinates. Compare all rows
            # in the engine against the original 9-digit literals instead.
            if int(fetch(cur, f"SELECT COUNT(*) FROM `{table}`")[0][0]) != len(base):
                raise ValueError("data import row count mismatch")
            for first in range(0, len(base), 100):
                end = min(first + 100, len(base))
                cases = " ".join(f"WHEN {int(ids[i])} THEN %s" for i in range(first, end))
                id_list = ",".join(str(int(ids[i])) for i in range(first, end))
                checked = fetch(cur, f"SELECT id,l2_distance(v,CASE id {cases} END) FROM `{table}` WHERE id IN ({id_list})",
                                [vector_literal(base[i]) for i in range(first, end)])
                if {int(x[0]) for x in checked} != {int(x) for x in ids[first:end]} or any(float(x[1]) != 0.0 for x in checked):
                    raise ValueError("stored vector/ID differs from original vector literal")
            record["all_imported_vectors_checked_by_zero_l2"] = True
            ddl = (f"CREATE VECTOR INDEX ann ON `{table}`(v) WITH "
                   f"(distance=l2,type=hnsw,lib={lib},m={config.m},ef_construction={config.ef_construction},ef_search={config.ef_search})")
            record["ddl"] = ddl
            literal = vector_literal(queries[0])
            sql = query_sql(table, config.k)
            # No warm-up against this index before the timed phase.
            record["build_started_utc"] = datetime.now(timezone.utc).isoformat()
            start = time.perf_counter()
            cur.execute(ddl)
            after_ddl = time.perf_counter()
            record["ddl_finished_utc"] = datetime.now(timezone.utc).isoformat()
            first_rows = fetch(cur, sql, (literal, literal))
            end = time.perf_counter()
            record["first_query_finished_utc"] = datetime.now(timezone.utc).isoformat()
            record.update(ddl_seconds=after_ddl-start, first_query_seconds=end-after_ddl,
                          observed_build_plus_first_query_seconds=end-start)
            record["first_query"] = [neighbor_record(x[0], x[1]) for x in first_rows]
            # The plan is checked without adding EXPLAIN time to either timed build.
            plan = fetch(cur, "EXPLAIN " + sql, (literal, literal))
            plan_text = "\n".join(" | ".join(str(cell) for cell in row) for row in plan)
            record["explain"] = plan_text
            if "VECTOR INDEX" not in plan_text.upper():
                raise ValueError("no VECTOR INDEX operator in query plan; refuse to count a full scan")
            results = []
            query_start = time.perf_counter()
            for i, query in enumerate(queries):
                lit = vector_literal(query)
                rows = fetch(cur, sql, (lit, lit))
                results.append({"query_id": i, "neighbors": [neighbor_record(nid, distance) for nid, distance in rows]})
            record["validation_query_seconds"] = time.perf_counter() - query_start
            record["metrics"] = score_results(truth, results, ids)
            record["query_results"] = results
            record["index_catalog"] = [[str(x) for x in row] for row in fetch(cur, f"SHOW INDEX FROM `{table}`")]
            record["status"] = "sql_smoke_passed" if record["metrics"]["valid_results"] else "invalid_results"
            record["notes"] = ["First-query timing includes any work it triggered; later lazy builds still need tracing.",
                               "SQL success/Recall and a VECTOR INDEX plan do not prove full construction or GPU execution.",
                               "No official performance/quality target is assumed. CPU and GPU must both meet it."]
    except Exception as exc:
        # Avoid connection strings/passwords; server errors retain error class/errno only.
        record["status"] = "failed"
        record["error_type"] = type(exc).__name__
        if isinstance(exc, ValueError):
            record["error"] = str(exc)
        elif exc.args and isinstance(exc.args[0], int):
            record["sql_errno"] = exc.args[0]
    finally:
        write_json(destination, record)
    print(table, record["status"], flush=True)
    return record


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dataset", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--backend", choices=["cpu", "gpu", "both"], default="cpu")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=2881)
    p.add_argument("--user", default="root")
    p.add_argument("--m", type=int, default=16)
    p.add_argument("--ef-construction", type=int, default=200)
    p.add_argument("--ef-search", type=int, default=128)
    p.add_argument("--k", type=int, default=10)
    a = p.parse_args()
    if a.rounds < 1 or min(a.m, a.ef_construction, a.ef_search, a.k) <= 0:
        p.error("positive counts/parameters required")
    manifest, base, queries, ids, truth = load_dataset(a.dataset)
    if a.k != manifest["k"]:
        p.error("k must match the exact ground truth")
    from mysql_client import pymysql
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=False)
    database = "amd_hnsw_" + uuid.uuid4().hex[:12]
    summary = {"database": database, "dataset": manifest,
               "parameters": {"m": a.m, "ef_construction": a.ef_construction, "ef_search": a.ef_search, "k": a.k},
               "report_kind": "preliminary_measurements_not_competition_score", "rounds": [],
               "official_speedup": None, "reason": "GPU execution and complete build evidence not yet audited"}
    with pymysql.connect(host=a.host, port=a.port, user=a.user, password=os.environ.get("SEEKDB_PASSWORD", ""),
                         autocommit=True, connect_timeout=15, read_timeout=1800, write_timeout=120) as conn:
        with conn.cursor() as cur:
            summary["server_version"] = fetch(cur, "SELECT VERSION()")[0][0]
            cur.execute(f"CREATE DATABASE `{database}`")
            cur.execute(f"USE `{database}`")
        write_json(out / "summary.json", summary)
        for iteration in range(1, a.rounds + 1):
            backends = (["cpu", "gpu"] if iteration % 2 else ["gpu", "cpu"]) if a.backend == "both" else [a.backend]
            for backend in backends:
                row = run_round(conn, backend, iteration, a, base, queries, ids, truth, out)
                summary["rounds"].append({"table": row["table"], "backend": backend, "status": row["status"],
                                          "seconds": row.get("observed_build_plus_first_query_seconds"),
                                          "recall": row.get("metrics", {}).get("recall_at_k")})
                write_json(out / "summary.json", summary)
                if row["status"] != "sql_smoke_passed":
                    raise SystemExit("Stopped after failure; evidence and isolated test database are retained.")
        summary["observed_medians"] = {backend: statistics.median([r["seconds"] for r in summary["rounds"] if r["backend"] == backend])
                                       for backend in set(r["backend"] for r in summary["rounds"])}
        write_json(out / "summary.json", summary)
    print("Raw measurements saved; no GPU speedup claim has been made.")


if __name__ == "__main__":
    main()
