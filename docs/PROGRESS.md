# Development progress

Updated 2026-10-28. The integrated firmware now has an ESP-Mosaico ESP32-S31
profile alongside the existing S3 profiles. Current development is on
[`codex/s31-mosaico-port`](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/s31-mosaico-port);
`main` and the older AMOLED branch retain their recorded scopes. All variants
remain experimental. The results below distinguish local checks, named field
builds and the remaining hardware gates.

## ESP-Mosaico port (2026-10-28)

The `mosaico` profile reuses the 480 x 480 AMOLED UI and integrated Hub/grinder
workflow on ESP32-S31, with 16 MB NOR flash and 16 MB PSRAM. Its board driver
uses the Mosaico display/touch mapping, BMI270 orientation, read-only BQ27220
battery observations and ES8311 audio. The BSP's board-revision detection is
retained; the port does not rewrite battery calibration or use NAND storage.
See the [build and installation guide](MOSAICO.md).

- The S31 profile uses a separate ESP-IDF 6.2 preview checkout and pinned board
  dependencies. S3 profiles retain ESP-IDF 5.5.4. Full S31 builds and an isolated
  S3 `amoled216` regression build passed; IDF 5/6 host compatibility was checked.
- Initial USB installation and subsequent authenticated LAN OTA succeeded.
  The final installed build `8d4984d3` passed four automatic BLE temperature
  refreshes over 100 seconds, with Cloud and grinder ready. A successful manual
  BLE read now releases an earlier recovery backoff so normal polling resumes.
- The user confirmed four-direction rotation and matching touch on preceding
  build `0436954d`, and sound on an earlier S31 build. The nearby-machine test
  used USB power from an external power bank, with a matching charging indication;
  battery-only operation has not been validated. These observations are separate
  from final-build acceptance.
- A new S31 cup with recipe writes and acknowledged feedback, extended battery
  runtime, charging transitions and prolonged mixed BLE/TLS/display operation
  remain unverified. Larger PSRAM and successful builds are not performance claims.

Latest Cloud coffee measurements, current target-dose selection and this
device's successful feedback journal remain separate data. A new board can
read an earlier measured cup while having no local successful deliveries or
confirmed target selection; a historical measured weight does not select a
target or imply that the new board replayed the cup.

## AMOLED adaptation and project overview

The original 480 x 480 S3 AMOLED adaptation is public on
[`codex/amoled-216-local-adaptation`](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/amoled-216-local-adaptation).
Current shared AMOLED maintenance is included in `codex/s31-mosaico-port`.
Neither board adaptation has been merged into `main`.
The [compatibility matrix](COMPATIBILITY.md) identifies the source and evidence
for each board. The [English overview](../README.md) includes S31. The
[Chinese](../README.zh-CN.md) and [German](../README.de.md) introductions now
link to the new S31 support in an English notice; their remaining text retains
the earlier S3 scope.

The AMOLED work adds optional rotation with matched touch, battery-session
behavior, retained scale battery history, compact boiler warm-up estimates and
clearer per-node cues. Real cups and audio were accepted on `f8b54b1a`;
`211d2963` then corrected manual-weight display precedence after a completed cup
and passed local UI, OTA and startup checks. Its physical consecutive-cup
selection and the next natural cold-start boiler estimates remain pending.
See the [exact-build status](AMOLED216.md#validation-status-2026-10-23).

## Earlier S3 milestone: one device and automatic recipe following

- The public `firmware/integrated` application includes the Hub, native UI,
  grinder worker, local controls, history/schedule caches and authenticated LAN
  OTA. A private repository or second ESP is no longer a build/runtime dependency
  for this profile. Cloud access is still required.
- After fresh job acceptance, one ordinary log export provides the recipe's
  target beverage weight by exact grind UUID. Existing guarded preset commands
  select it during puck preparation, preserving manual cancellation and unknown
  outcome handling. No database or hardcoded handle mapping is used.
- PSRAM allocation increases internal memory headroom. Fresh command reads
  replace a stale display-cache rejection, and immediate log lookup avoids an
  extra Cloud poll before setting the target.
- The integrated home preserves the approved Noto/Inter typography and palette,
  four quick weights, compact navigation and direct linkage/history access.
  Steam shows a large level 1/2/3 with measured/target temperatures below. Touch
  matches the physical USB-C-right orientation; confirmed dose selection persists.
- A source-build cup passed automatic 20 g -> 30 g before the lever, measured
  30.2 g / 27.696 s feedback and physical grinder adjustment. The user also
  confirmed touch behavior. A second cup on the same `c88791b2` build passed
  automatic 30 g -> 20 g with 20.1 g / 25.053 s acknowledged feedback; 192 samples
  had no read failures. The user reported normal completion but did not separately
  confirm lever timing or physical adjustment for that cup. Both directions now
  have one successful cup, not repeated-alternation acceptance. See the
  [exact-build report](RECIPE_FOLLOW_ACCEPTANCE.md).
- The reverse cup exposed stale `scaleConnected: false` despite a connected scale
  and valid measured result. The shared cache now accepts fresh control preflight
  observations, uses bounded 30-second accessory reads and expires observations
  after 60 seconds. Missing fields remain unknown. The authorized OTA of source
  `975ebc5`, build `b923fa91`, subsequently passed a physical off -> on -> off
  cycle: disconnected/unknown battery -> connected/28% -> disconnected/unknown
  battery. Over 252 seconds, 174 cached reads had no failures, uptime remained
  monotonic and the grinder remained ready with its success count unchanged at
  12. No cup was made on this new build. See [the scale report](SCALE_STATE_ACCEPTANCE.md).

## Build and validation boundaries

| Surface | Evidence and status |
| --- | --- |
| ESP-Mosaico S31 | `8d4984d3`: automatic BLE temperature refresh and Cloud/grinder readiness. Earlier S31 builds: rotation/touch and sound. Complete new-cup writes, endurance and charging gates remain open. |
| S3 AMOLED on the current development branch | Isolated full ESP-IDF 5.5.4 build passed without changing the active S31 dependency tree or flashing the S3 board. Earlier S3 hardware results remain build-specific. |
| Integrated touchscreen source | `c88791b2` has one successful cup in each direction. Later `b923fa91` passed OTA and one scale off/on/off cycle on the 1.69-inch board; a new-build cup, endurance and physical touch latency remain unverified. |
| Integrated headless | Shared Hub/grinder core with no display tasks. Dedicated integrated-headless hardware acceptance remains open. |
| Root companion and external-Hub preview | Existing source/features retained; job/coffee parser compatibility fixes synchronized. Their older hardware evidence does not certify the integrated build. |
| Host regressions | Both offline entry points passed locally: synthetic protocol/worker/UI and Hub/recipe/transaction checks, with ASan/UBSan and no real devices. |
| Earlier S3 builds and simulators | The four original profiles built: root headless/touchscreen and integrated headless/touchscreen. Both native simulator self-tests passed. The initial public import's runtime C/headers matched the archived source. |
| Initial provisioning | Imported identity starts in monitor mode. Actual NVS generation is tested at both touchscreen/headless sizes, with private file permissions and no credential echo. |
| Build portability | S3 integrated manifests pin ESP-IDF 5.5.4; Mosaico uses 6.2 preview. Host checks locate cJSON in IDF 5 or the IDF 6 managed dependency. The older 5.5.0 image lacks a required WebSocket transport API. |
| CI configuration | The [workflow](../.github/workflows/ci.yml) retains the root S3 builds, adds S3 AMOLED to the IDF 5.5.4 integrated job, and adds a separate S31 job pinned to the validated IDF revision. The new jobs have not yet been run remotely; the build evidence above is local. |
| Production release/security | Still experimental. No general firmware release, armed eFuse tool or security-gate certification is part of this publication. |

The S3 integrated CI job includes host and crypto checks, LCD/AMOLED simulator
self-tests and `touchscreen`, `headless` and `amoled216` firmware builds. The
S31 job uses its own checkout and the IDF revision documented in
[MOSAICO.md](MOSAICO.md), builds `mosaico` before host/crypto checks to obtain
cJSON, and uses public synthesized sound cues. A configured job is not a
successful hosted run, and a successful build would not establish hardware acceptance.

For the scale-cache correction, both full offline entry points passed with
ASan/UBSan, including the real keeper loop, scale parser/publication/JSON and
bounded background-read error paths. Local ESP-IDF builds passed for integrated
touchscreen, integrated headless and the root headless companion. No layout or
interaction changed; this round did not repeat simulator or physical UI checks.
That scale-cache round predates the AMOLED board/display work now included in
the current development branch; its LCD hardware evidence does not certify
either AMOLED board.

The earlier simulator drag, daily controls, schedule editing and multi-cup
history work is retained; its historical report is available in Git at
`423e086:docs/PROGRESS.md`. The root UI uses an external Hub, while integrated
UI calls the same board's Hub directly. Their setup guides must not be mixed.

## Remaining work, in order

1. Complete S31 new-cup recipe writes and feedback acceptance on the installed
   build, followed by battery-runtime and charging checks. Recheck rotation,
   touch and sound if subsequent changes affect those paths.
2. Verify a real cup on `b923fa91`, then repeat alternating-handle tests beyond
   the single successful cup in each direction on the earlier `c88791b2` build.
3. Scale-off weight changes retained at scale power-on and App/device display
   agreement. The completed off/on/off cycle did not issue preset commands.
4. Network/Cloud interruptions, hardware stale/unknown transitions,
   deadline/cancellation timing, physical touch latency and extended runtime
   with touch, BLE, TLS, log parsing and feedback active together.
5. A reliable live extraction-start signal. Current timer checks do not prove a
   physical extraction lock: wait for confirmed target weight before brewing.
6. Dedicated integrated-headless, E80 and other-board acceptance. Each board's
   hardware results remain separate from the 1.69-inch scale result.
7. Physical schedule edit/readback and remaining App/Widget/Watch/Siri
   matrices. Their application projects are maintained separately.
8. Production security/installation gates and upstream recipe-relative
   adjustment-window hints. Acknowledged feedback is distinct from adjustment.

Schedule creation/deletion, independent dim/blank timers and a full on-device
setup wizard remain separate backlog items. Existing-plan editing and LAN OTA
are already included in the integrated application.

## Upstream and source maintenance

Rust upstream was fetched and reviewed through `ebaac65` on 2026-10-22. Both C
parsers accept null/missing coffee lists as empty while rejecting invalid shapes,
and treat target durations below 1 ms as absent. Desktop-only features remain
reviewed differences, detailed in [UPSTREAM.md](UPSTREAM.md).

The integrated import keeps the deployed source layout and attribution. Later
shared scale-cache maintenance is separate from the archived runtime. See its
[source boundary](../firmware/integrated/UPSTREAM.md). No private configuration,
certificates, device identities, raw logs or firmware binaries are published.
