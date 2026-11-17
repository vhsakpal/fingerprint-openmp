#!/usr/bin/env python3
"""Fast integration tests, including reading a real .xyt-style manifest."""
import csv
import math
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / ("fingerprint_omp.exe" if os.name == "nt" else "fingerprint_omp")


def run(*args):
    out = subprocess.run([str(EXE), *map(str,args)],cwd=ROOT,
                         capture_output=True,text=True,check=True,timeout=120)
    return out.stdout


def parse_csv(path):
    with open(path,newline="",encoding="utf8") as f:
        return list(csv.DictReader(f))


def main():
    global EXE
    with tempfile.TemporaryDirectory() as t:
        tmp=Path(t)
        EXE = tmp / ("fingerprint_omp.exe" if os.name == "nt" else "fingerprint_omp")
        subprocess.run([os.environ.get("CC", "gcc"), "-O2", "-std=c11",
                        "-Wall", "-Wextra", "-Wpedantic", "-fopenmp",
                        str(ROOT/"fingerprint_omp.c"), "-o", str(EXE), "-lm"],
                       check=True, timeout=120)
        for schedule in ("static","dynamic","guided"):
            for early in (0,1):
                csv_path=tmp / "r.csv"
                tcsv=tmp / "t.csv"
                run("--synthetic",100,"--query-index",30,"--threads",4,
                    "--schedule",schedule,"--early",early,"--threshold",100,
                    "--repeats",2,"--csv",csv_path,"--thread-csv",tcsv)
                rows=parse_csv(csv_path)
                assert len(rows)==2
                assert all(r["same_identity"]=="1" for r in rows), rows
                assert all(r["match_score"]=="100" for r in rows), rows
                assert all(1 <= int(r["checked"]) <=100 for r in rows)
                assert all(math.isfinite(float(r["parallel_seconds"])) and
                           float(r["parallel_seconds"]) > 0 for r in rows), rows
                assert all(math.isfinite(float(r["sequential_seconds"])) and
                           float(r["sequential_seconds"]) > 0 for r in rows), rows
                assert all(int(r["actual_threads"]) == 4 for r in rows), rows
                if early:
                    assert all(r["speedup"] == "" and r["efficiency"] == "" for r in rows), rows
                    assert all(int(r["checked"]) >= 1 and
                               int(r["match_score"]) >= int(r["threshold"]) and
                               r["accepted"] == "1" for r in rows), rows
                else:
                    assert all(int(r["checked"]) == 100 and
                               r["same_exact_best"] == "1" for r in rows), rows
                    assert all(float(r["speedup"]) > 0 and
                               float(r["efficiency"]) > 0 for r in rows), rows
                assert len(parse_csv(tcsv))==8
        xyt=tmp / "xyt"
        xyt.mkdir()
        files=[]
        for i in range(8):
            p=xyt / f"finger{i}.xyt"
            p.write_text(f"{20+i*30} {20+i*30} 45 80\n{40+i*30} {60+i*30} 90 90\n",encoding="utf8")
            files.append(p)
        manifest=tmp / "manifest.txt"
        manifest.write_text("\n".join(map(str,files))+"\n",encoding="utf8")
        run("--manifest",manifest,"--query-index",3,"--threads",2,
            "--schedule","dynamic","--early",1,"--repeats",1,
            "--csv",tmp/"manifest.csv","--thread-csv",tmp/"manifest_threads.csv")
        row=parse_csv(tmp/"manifest.csv")[0]
        assert row["reference_identity"]=="3"
        assert row["match_identity"]=="3"
        assert row["same_exact_best"]=="1"

        # A query valid in the base must wrap into the smaller benchmark gallery.
        large_manifest = tmp / "large_manifest.txt"
        large_manifest.write_text((str(files[0])+"\n")*6001, encoding="utf8")
        output = run("--benchmark", "--manifest", large_manifest,
                     "--query-index", 6000, "--repeats", 1,
                     "--csv", tmp/"benchmark.csv",
                     "--thread-csv", tmp/"benchmark_threads.csv")
        assert "Query: gallery index 0, identity 0" in output
        rows = parse_csv(tmp/"benchmark.csv")
        assert len(rows) == 90
        assert all(r["match_score"] == "100" for r in rows)
        assert all(r["same_exact_best"] == "1" for r in rows if r["early_stop"] == "0")

        # Opposite signed-short coordinates previously overflowed squared distance.
        far = tmp / "far.xyt"
        near = tmp / "near.xyt"
        far.write_text("-32768 -32768 0 80\n", encoding="utf8")
        near.write_text("32767 32767 0 80\n", encoding="utf8")
        edge_manifest = tmp / "edge_manifest.txt"
        edge_manifest.write_text(str(far)+"\n", encoding="utf8")
        run("--manifest", edge_manifest, "--query-file", near,
            "--threads", 2, "--early", 0, "--repeats", 1,
            "--csv", tmp/"edge.csv", "--thread-csv", tmp/"edge_threads.csv")
        row = parse_csv(tmp/"edge.csv")[0]
        assert row["match_score"] == "0" and row["reference_score"] == "0"
    print("All OpenMP smoke tests passed")


if __name__=="__main__":
    main()
