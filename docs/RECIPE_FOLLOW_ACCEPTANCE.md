# Recipe following: field result, 2026-10-22

One complete automatic-follow cup passed on the integrated source build. The
user confirmed target selection before pulling the lever, physical grinder
adjustment after the result and normal touchscreen operation. This public
publication involves no further device access or firmware installation.

## Setup and result

The setup used one Waveshare ESP32-S3-Touch-LCD-1.69 (16 MB flash / 8 MB PSRAM),
an E65W on installed V01.36 firmware and a Linea Mini R with its scale. The ESP
owned the Hub, grinder workflow and native screen. Cloud controls/results were
still required; the computer only observed cached diagnostics during the cup.

| Step | Observed result |
| --- | --- |
| Initial state | Machine manually set to Dose1 / 20 g; selected handle recipe targeted 30 g. |
| Fresh accepted grind | One ordinary Owner-authenticated log export, matched by the current grind UUID. No development-firmware unlock or database export. |
| Log lookup | 37,338-byte ZIP, 848 CSV rows; login/download/parse took 4,901 ms. |
| Automatic preset command | One command, one Cloud write, 5,581 ms; selected existing Dose2 / 30 g. |
| Preparation window | First sampled 30 g state about 17 seconds after the CSV grind timestamp, including publication/polling and approximately 2-second sample resolution. User confirmed 30 g before the lever. |
| Shot and feedback | Measured 30.2 g / 27.696 s; `brewDone` acknowledged and the user observed grinder adjustment. |
| Final state | Ready, no active/pending task; 102 diagnostic samples with zero read failures. Touch-read errors remained zero. |

4.901 seconds is the log stage, not full end-to-end latency. This is one observed
preparation window, not a latency guarantee. A grinder acknowledging a result
also does not prove that every future shot will be eligible for adjustment.

An earlier attempt matched 30 g but was rejected before writing because a new
120-second display-cache freshness check conflicted with the Hub's longer
background refresh interval. The user manually selected 30 g for that cup;
its 30.1 g / 35.738 s feedback does **not** count as automatic-follow success.
The fix moved the online/preset check into the existing fresh command read.
The final version also starts log lookup immediately after successful acceptance,
before an unnecessary extra Cloud result poll.

## Memory and transaction bounds

Large existing event/command/configuration state moved to PSRAM. Observed free
internal RAM rose from 10,327 bytes before this work to 30,279 bytes after the
successful cup; the largest final internal block was 15,872 bytes. These are
observations, not an exhaustive peak or endurance measurement.

The implementation caps ZIP input at 1 MiB and decompressed CSV at 2 MiB, uses a
32 KiB PSRAM inflate window, verifies CRC and accepts only a unique successful,
GbS-enabled row with a whole-gram target in 1..200 g. It reads once per fresh
accepted task. Writes retain a 15-second task permit, at most 8-second command
deadline, manual cancellation and no replay on ambiguous outcomes.

## Exact source-build identity

- Build ID: `c88791b2`; original build version `7b00f00a-dirty`.
- Application size: 2,336,576 bytes.
- Application SHA-256: `66cc6cc7d91f96707b7c6719b94a27e89873f6504fdc7a369be51419097cab01`.
- ELF SHA-256: `c88791b2bad708a01501548bcc953f51cad4c6bad103c6b56f204fdcf8fc71af`.
- Runtime source subsequently archived at `050790dc62b0c121da70911652f108e049b40754`
  and imported into this repository's `firmware/integrated` application.

Public path/version/build metadata changes produce a different binary. This
report identifies the source build actually tested; local builds and CI are
separate evidence, not a second cup or a universal hardware acceptance claim.
Raw device logs, network addresses, credentials, certificates and task IDs are
not published. The [upstream field update](https://github.com/Skamba/gbs-anywhere/pull/15#issuecomment-6331112363)
shares the same measured result and limitations.

## Still open

1. Reverse automatic following to a 20 g recipe and alternating handles.
2. Network/Cloud faults, cancellation timing and long-duration coexistence.
3. A reliable real-time extraction-start input. The current timer check is not
   a proven physical extraction lock; wait for confirmed target selection.
4. Integrated headless hardware, E80 and other boards/firmware versions.
5. Scale-off selection retained at scale power-on, complete client agreement,
   physical schedule edit/readback and production security gates.
