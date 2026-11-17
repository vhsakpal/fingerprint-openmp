#!/usr/bin/env python3
"""Extract NIST MINDTCT .xyt minutiae from SOCOFing BMP images.

Requirements: Pillow, NBIS `cwsq` and `mindtct` executables in PATH.
MINDTCT does NOT accept BMP or ordinary PGM as direct input. Convert BMP to
raw grayscale, encode to WSQ using cwsq, then run mindtct on WSQ.

Example:
 python prepare_socofing.py --images SOCOFing/Real --output data/xyt --limit 6000
"""
from __future__ import annotations
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=Path, required=True, help="Path to SOCOFing/Real containing BMP files")
    parser.add_argument("--output", type=Path, default=Path("data/xyt"))
    parser.add_argument("--limit", type=int, default=6000)
    parser.add_argument("--ppi", type=int, default=500, help="Assumed pixels per inch; check source metadata")
    parser.add_argument("--cwsq", default="cwsq", help="Path to NBIS cwsq")
    parser.add_argument("--mindtct", default="mindtct", help="Path to NBIS mindtct")
    parser.add_argument("--keep-other-nbis", action="store_true", help="Retain intermediate NIST output reports")
    args = parser.parse_args()

    if args.limit <= 0:
        parser.error("--limit must be positive")
    if args.ppi <= 0:
        parser.error("--ppi must be positive")
    if not args.images.is_dir():
        parser.error(f"Image directory not found: {args.images}")
    for command in [args.cwsq, args.mindtct]:
        if not shutil.which(command):
            parser.error(f"NBIS executable not found in PATH: {command}")
    try:
        from PIL import Image
    except ImportError:
        parser.error("Pillow is missing. Install: python -m pip install pillow")

    files = sorted(p for p in args.images.rglob("*") if p.suffix.lower() == ".bmp")
    if not files:
        parser.error("No .BMP images found. Use the SOCOFing/Real folder")
    files = files[:args.limit]
    args.output.mkdir(parents=True, exist_ok=True)
    manifest_path = args.output / "manifest.txt"
    generated, failed = [], []

    with TemporaryDirectory(prefix="socofing_wsqs_") as tmp:
        temp = Path(tmp)
        for number, image_path in enumerate(files, 1):
            # Add relative directory information to avoid accidental name collisions.
            rel = image_path.relative_to(args.images)
            stem = "__".join(rel.with_suffix("").parts)
            raw = temp / (stem + ".raw")
            output_prefix = args.output / stem
            wsq = raw.with_suffix(".wsq")
            xyt = output_prefix.with_suffix(".xyt")
            try:
                with Image.open(image_path) as pic:
                    gray = pic.convert("L")
                    width, height = gray.size
                    raw.write_bytes(gray.tobytes())
                subprocess.run([args.cwsq, "0.75", "wsq", str(raw),
                                "-raw_in", f"{width},{height},8,{args.ppi}"],
                               check=True, capture_output=True, text=True)
                subprocess.run([args.mindtct, str(wsq), str(output_prefix)],
                               check=True, capture_output=True, text=True)
                if not xyt.exists() or not xyt.read_text(encoding="utf8",errors="replace").strip():
                    raise RuntimeError("MINDTCT did not produce nonempty .xyt minutiae")
                generated.append(xyt.resolve())
                if not args.keep_other_nbis:
                    for other in args.output.glob(stem + ".*"):
                        if other.suffix.lower() != ".xyt":
                            other.unlink(missing_ok=True)
            except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
                details = exc.stderr.strip() if isinstance(exc, subprocess.CalledProcessError) and exc.stderr else str(exc)
                failed.append((image_path.name, details))
                print(f"FAILED {image_path.name}: {details[:250]}", file=sys.stderr)
            finally:
                raw.unlink(missing_ok=True)
                wsq.unlink(missing_ok=True)
            if number % 100 == 0 or number == len(files):
                print(f"Processed {number}/{len(files)} | successful: {len(generated)} | failed: {len(failed)}")

    # Absolute paths make this manifest independent of the current working directory.
    manifest_path.write_text("".join(str(f) + "\n" for f in generated), encoding="utf8")
    print(f"Manifest: {manifest_path.resolve()} ({len(generated)} usable templates)")
    if failed:
        failfile = args.output / "failed.txt"
        failfile.write_text("".join(f"{name}\t{why}\n" for name, why in failed),encoding="utf8")
        print(f"Failures: {failfile.resolve()}")
        print("Note: SOCOFing images are small; MINDTCT may fail/under-detect on them.")
    return 0 if generated else 1


if __name__ == "__main__":
    raise SystemExit(main())
