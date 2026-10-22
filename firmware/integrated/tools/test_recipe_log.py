#!/usr/bin/env python3
"""Exercise the production ZIP/CSV parser with bounded, adversarial exports."""
import csv
import io
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
UUID = "11111111-1111-4111-8111-111111111111"
OTHER = "22222222-2222-4222-8222-222222222222"
HEAD = ["grind_uuid", "grind_date_time", "recipe_brew_weight", "grind_successfull", "recipe_gbs_enabled", "recipe_index", "grind_debug_data"]
ROW = [UUID, "2026-10-22T00:16:57Z", "30000", "1", "1", "1", "quoted,\ntext\"here"]


def archive(rows, method=zipfile.ZIP_DEFLATED, header=HEAD):
    text = io.StringIO(newline="")
    csv.writer(text).writerows([header, *rows])
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w", method) as z:
        z.writestr("HemroNg2GrindLogs_test.csv", text.getvalue())
        z.writestr("ServiceLogs_test.csv", "ignored")
    return data.getvalue()


def main():
    with tempfile.TemporaryDirectory(prefix="gbs-log-test-") as temp:
        tmp = Path(temp); exe = tmp / "parse"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
                        "-I", str(ROOT / "integrated/main"), str(ROOT / "integrated/main/recipe_log.c"),
                        str(ROOT / "integrated/tests/recipe_log_runner.c"), "-lz", "-o", str(exe)], check=True)
        cases = [(archive([ROW]), 30), (archive([ROW], zipfile.ZIP_STORED), 30),
                 (archive([ROW, [OTHER, *ROW[1:]]]), 30), (archive([[OTHER, *ROW[1:]]]), 0),
                 (archive([ROW, ROW]), 0), (archive([ROW], header=HEAD[:-1]), 0)]
        for column, value in [(2, "0"), (2, "30500"), (2, "200001"), (2, "-30000"), (2, "30g"), (3, "0"), (4, "0")]:
            row = ROW.copy(); row[column] = value; cases.append((archive([row]), 0))
        many = [[OTHER, *ROW[1:]] for _ in range(4999)]
        cases += [(archive([*many, ROW]), 30), (archive([*many, ROW, ROW]), 0), (archive([ROW])[:-8], 0)]
        broken = bytearray(archive([ROW], zipfile.ZIP_STORED)); broken[60] ^= 1
        cases.append((broken, 0))
        huge = ROW.copy(); huge[-1] = "x" * 4097; cases.append((archive([huge]), 0))
        for index, (data, expected) in enumerate(cases):
            path = tmp / f"{index}.zip"; path.write_bytes(data)
            subprocess.run([str(exe), str(path), UUID, str(expected)], check=True, stdout=subprocess.DEVNULL)
        print(f"recipe ZIP/CSV: {len(cases)} cases passed")


if __name__ == "__main__":
    main()
