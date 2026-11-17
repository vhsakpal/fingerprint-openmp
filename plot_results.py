#!/usr/bin/env python3
"""Plot validated OpenMP fingerprint benchmark results.

Run: python plot_results.py --results results.csv --threads thread_results.csv
Install matplotlib: python -m pip install matplotlib
"""
from __future__ import annotations
import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import mean


def read_csv(path: Path, required: set[str]) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf8") as file:
        reader = csv.DictReader(file)
        missing = required.difference(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{path} is missing columns: {', '.join(sorted(missing))}")
        return list(reader)


def finite_value(row: dict[str, str], column: str) -> float:
    value = float(row[column])
    if not math.isfinite(value):
        raise ValueError(f"Non-finite {column} value in benchmark CSV")
    return value


def validate_results(rows: list[dict[str, str]]) -> None:
    if not rows:
        raise ValueError("The results CSV is empty")
    for row in rows:
        n = int(row["db_size"])
        early = int(row["early_stop"])
        parallel = finite_value(row, "parallel_seconds")
        sequential = finite_value(row, "sequential_seconds")
        checked = int(row["checked"])
        score = int(row["match_score"])
        threshold = int(row["threshold"])
        if parallel <= 0 or sequential <= 0:
            raise ValueError("Benchmark execution times must be positive")
        if not 1 <= checked <= n:
            raise ValueError("Comparison count is outside the database size")
        if early:
            if score < threshold or row["accepted"] != "1":
                raise ValueError("Early stop reported a match without meeting its threshold")
        elif checked != n or row["same_exact_best"] != "1":
            raise ValueError("Full-search result did not check the full database and match the reference")


def mean_time(rows: list[dict[str, str]], column: str) -> float:
    return mean(finite_value(row, column) for row in rows)


def save_figure(fig, path: Path) -> None:
    fig.tight_layout()
    fig.savefig(path, dpi=160)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, default=Path("results.csv"))
    parser.add_argument("--threads", type=Path, default=Path("thread_results.csv"))
    parser.add_argument("--output", type=Path, default=Path("plots"))
    args = parser.parse_args()
    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import StrMethodFormatter
    except ImportError:
        parser.error("matplotlib missing: python -m pip install matplotlib")

    try:
        rows = read_csv(args.results, {
            "db_size", "threads", "actual_threads", "schedule", "early_stop",
            "run", "parallel_seconds", "sequential_seconds", "checked",
            "match_score", "threshold", "accepted", "same_exact_best",
        })
        validate_results(rows)
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))

    args.output.mkdir(parents=True, exist_ok=True)
    by_config: dict[tuple[int, int, str, int], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        key = (int(row["db_size"]), int(row["threads"]),
               row["schedule"], int(row["early_stop"]))
        by_config[key].append(row)
    sizes = sorted({key[0] for key in by_config})
    schedules = [name for name in ("static", "dynamic", "guided")
                 if any(key[2] == name for key in by_config)]

    for n in sizes:
        threads = sorted({key[1] for key in by_config if key[0] == n})
        full_search = {
            (t, schedule): by_config[(n, t, schedule, 0)]
            for t in threads for schedule in schedules
            if (n, t, schedule, 0) in by_config
        }

        for metric in ("speedup", "efficiency"):
            fig, ax = plt.subplots(figsize=(8, 5))
            for schedule in schedules:
                xs, ys = [], []
                for t in threads:
                    samples = full_search.get((t, schedule))
                    if not samples:
                        continue
                    speedup = mean_time(samples, "sequential_seconds") / mean_time(samples, "parallel_seconds")
                    actual_threads = mean(int(row["actual_threads"]) for row in samples)
                    xs.append(t)
                    ys.append(speedup if metric == "speedup" else speedup / actual_threads)
                if xs:
                    ax.plot(xs, ys, marker="o", label=schedule)
            if metric == "speedup":
                ax.plot(threads, threads, linestyle=":", color="black", label="Ideal speedup")
                ylabel = "Speedup vs sequential (x)"
            else:
                ylabel = "Efficiency = speedup / actual threads"
            ax.set(title=f"{ylabel} | {n:,} templates", xlabel="Requested OpenMP threads", ylabel=ylabel)
            ax.set_xticks(threads)
            ax.grid(alpha=.25)
            ax.legend()
            save_figure(fig, args.output / f"{metric}_{n}.png")
            plt.close(fig)

        fig, ax = plt.subplots(figsize=(8, 5))
        for schedule in schedules:
            xs, ys = [], []
            for t in threads:
                full = by_config.get((n, t, schedule, 0))
                early = by_config.get((n, t, schedule, 1))
                if full and early:
                    full_seconds = mean_time(full, "parallel_seconds")
                    early_seconds = mean_time(early, "parallel_seconds")
                    xs.append(t)
                    ys.append(100 * (full_seconds - early_seconds) / full_seconds)
            if xs:
                ax.plot(xs, ys, marker="o", label=schedule)
        ax.axhline(0, linewidth=.8, color="black")
        ax.set(title=f"Parallel time saved by early stopping | {n:,} templates",
               xlabel="Requested OpenMP threads", ylabel="Time saved (%)")
        ax.set_xticks(threads)
        ax.grid(alpha=.25)
        ax.legend()
        save_figure(fig, args.output / f"early_stop_savings_{n}.png")
        plt.close(fig)

        fig, ax = plt.subplots(figsize=(8, 5))
        for schedule in schedules:
            xs, ys = [], []
            for t in threads:
                samples = by_config.get((n, t, schedule, 1))
                if samples:
                    xs.append(t)
                    ys.append(mean_time(samples, "sequential_seconds") /
                              mean_time(samples, "parallel_seconds"))
            if xs:
                ax.plot(xs, ys, marker="o", label=schedule)
        ax.set(title=f"End-to-end speedup with early stopping | {n:,} templates",
               xlabel="Requested OpenMP threads", ylabel="Sequential full search / parallel early stop (x)")
        ax.set_xticks(threads)
        ax.grid(alpha=.25)
        ax.legend()
        save_figure(fig, args.output / f"end_to_end_speedup_{n}.png")
        plt.close(fig)

        fig, ax = plt.subplots(figsize=(8, 5))
        for schedule in schedules:
            xs, ys = [], []
            for t in threads:
                samples = full_search.get((t, schedule))
                if samples:
                    xs.append(t)
                    ys.append(mean_time(samples, "parallel_seconds"))
            if xs:
                ax.plot(xs, ys, marker="o", label=schedule)
        ax.set(title=f"Execution time comparison (early stopping OFF) | {n:,} templates",
               xlabel="Requested OpenMP threads", ylabel="Parallel execution time (seconds)")
        ax.set_xticks(threads)
        ax.grid(alpha=.25)
        ax.legend()
        save_figure(fig, args.output / f"execution_time_comparison_{n}.png")
        plt.close(fig)

        fig, ax = plt.subplots(figsize=(9, 5))
        for schedule in schedules:
            for early, style in ((0, "-"), (1, "--")):
                xs, ys = [], []
                for t in threads:
                    samples = by_config.get((n, t, schedule, early))
                    if samples:
                        xs.append(t)
                        ys.append(mean(int(row["checked"]) for row in samples))
                if xs:
                    ax.plot(xs, ys, linestyle=style, marker="o",
                            label=f"{schedule}, early stop {'ON' if early else 'OFF'}")
        ax.set(title=f"Number of fingerprint comparisons | {n:,} templates",
               xlabel="Requested OpenMP threads", ylabel="Comparisons per search")
        ax.set_xticks(threads)
        ax.yaxis.set_major_formatter(StrMethodFormatter("{x:,.0f}"))
        ax.set_ylim(bottom=0, top=n * 1.05)
        ax.grid(alpha=.25)
        ax.legend(fontsize=8)
        save_figure(fig, args.output / f"comparisons_early_stop_{n}.png")
        plt.close(fig)

    for t in sorted({key[1] for key in by_config}):
        fig, ax = plt.subplots(figsize=(8, 5))
        for schedule in schedules:
            xs, ys = [], []
            for n in sizes:
                samples = by_config.get((n, t, schedule, 0))
                if samples:
                    xs.append(n)
                    ys.append(mean_time(samples, "parallel_seconds"))
            if xs:
                ax.plot(xs, ys, marker="o", label=schedule)
        ax.set(title=f"Database size vs execution time | T={t}, early stopping OFF",
               xlabel="Database size (fingerprints)", ylabel="Parallel execution time (seconds)")
        ax.set_xticks(sizes)
        ax.xaxis.set_major_formatter(StrMethodFormatter("{x:,.0f}"))
        ax.grid(alpha=.25)
        ax.legend()
        save_figure(fig, args.output / f"database_size_vs_execution_time_T{t}.png")
        plt.close(fig)

    if args.threads.exists():
        try:
            thread_rows = read_csv(args.threads, {
                "db_size", "threads", "actual_threads", "schedule", "early_stop",
                "run", "thread_id", "busy_seconds", "idle_plus_overhead_est_seconds",
            })
            if thread_rows:
                max_n = max(int(row["db_size"]) for row in thread_rows)
                candidates = sorted({int(row["threads"]) for row in thread_rows
                                     if int(row["db_size"]) == max_n})
                chosen_threads = next((value for value in candidates if value == 4), candidates[-1])
                for column, filename, title, ylabel in (
                    ("busy_seconds", f"thread_balance_{max_n}_T{chosen_threads}.png",
                     "Per-thread matching time", "Seconds spent in match_score"),
                    ("idle_plus_overhead_est_seconds",
                     f"thread_idle_estimate_{max_n}_T{chosen_threads}.png",
                     "Estimated idle + overhead", "Wall time minus matching time (seconds)"),
                ):
                    fig, ax = plt.subplots(figsize=(8, 5))
                    found = False
                    for schedule in schedules:
                        subset = [row for row in thread_rows
                                  if int(row["db_size"]) == max_n
                                  and int(row["threads"]) == chosen_threads
                                  and row["schedule"] == schedule
                                  and int(row["early_stop"]) == 0
                                  and int(row["run"]) == 1]
                        if subset:
                            found = True
                            subset.sort(key=lambda row: int(row["thread_id"]))
                            ax.plot([int(row["thread_id"]) for row in subset],
                                    [finite_value(row, column) for row in subset],
                                    marker="o", label=schedule)
                    if found:
                        ax.set(title=f"{title} | N={max_n:,}, T={chosen_threads}, early stopping OFF",
                               xlabel="Thread ID", ylabel=ylabel)
                        ax.grid(alpha=.25)
                        ax.legend()
                        save_figure(fig, args.output / filename)
                    plt.close(fig)
        except (OSError, ValueError, KeyError) as error:
            parser.error(str(error))

    print(f"Plots saved in {args.output.resolve()}")


if __name__ == "__main__":
    main()
