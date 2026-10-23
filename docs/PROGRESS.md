# Development progress

Updated 2026-10-23. The complete integrated firmware is published in this
repository; the original lightweight and external-Hub display builds remain
available. This update records both directions of real-cup recipe following,
followed by the authorized 1.69-inch OTA and scale off/on/off acceptance. All
variants remain experimental. This documentation publication performs no new
device access or update.

## AMOLED adaptation and project overview

The 480 x 480 AMOLED adaptation is public on
[`codex/amoled-216-local-adaptation`](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/amoled-216-local-adaptation).
It has not yet been merged into `main`; this documentation update publishes
its overview and synthetic screenshots without changing the runtime on `main`.
The [compatibility matrix](COMPATIBILITY.md) identifies the source and evidence
for each board. [English](../README.md), [Chinese](../README.zh-CN.md) and
[German](../README.de.md) introductions share the same feature boundaries.

The AMOLED work adds optional rotation with matched touch, battery-session
behavior, retained scale battery history, compact boiler warm-up estimates and
clearer per-node cues. Real cups and audio were accepted on `f8b54b1a`;
`211d2963` then corrected manual-weight display precedence after a completed cup
and passed local UI, OTA and startup checks. Its physical consecutive-cup
selection and the next natural cold-start boiler estimates remain pending.
See the [exact-build status](AMOLED216.md#validation-status-2026-10-23).

## Latest milestone: one device and automatic recipe following

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
| Integrated touchscreen source | `c88791b2` has one successful cup in each direction. Later `b923fa91` passed OTA and one scale off/on/off cycle on the 1.69-inch board; a new-build cup, endurance and physical touch latency remain unverified. |
| Integrated headless | Shared Hub/grinder core with no display tasks. Dedicated integrated-headless hardware acceptance remains open. |
| Root companion and external-Hub preview | Existing source/features retained; job/coffee parser compatibility fixes synchronized. Their older hardware evidence does not certify the integrated build. |
| Host regressions | Both offline entry points passed locally: synthetic protocol/worker/UI and Hub/recipe/transaction checks, with ASan/UBSan and no real devices. |
| Local builds and simulators | All four profiles built: root headless/touchscreen and integrated headless/touchscreen. Both native simulator self-tests passed. The initial public import's runtime C/headers matched the archived source. |
| Initial provisioning | Imported identity starts in monitor mode. Actual NVS generation is tested at both touchscreen/headless sizes, with private file permissions and no credential echo. |
| Build portability | Host tests use `IDF_PATH`; the integrated manifest/CI pin ESP-IDF 5.5.4. The older 5.5.0 image lacks a WebSocket transport API used by the pinned component. |
| CI | The [workflow](../.github/workflows/ci.yml) checks both applications and all four main firmware profiles. Its commit-specific result is the hosted evidence; builds do not establish physical acceptance. |
| Production release/security | Still experimental. No general firmware release, armed eFuse tool or security-gate certification is part of this publication. |

For the scale-cache correction, both full offline entry points passed with
ASan/UBSan, including the real keeper loop, scale parser/publication/JSON and
bounded background-read error paths. Local ESP-IDF builds passed for integrated
touchscreen, integrated headless and the root headless companion. No layout or
interaction changed; this round did not repeat simulator or physical UI checks.
The scale contract is shared with the separate AMOLED development branch; that
branch's board/display work is not included here.

The earlier simulator drag, daily controls, schedule editing and multi-cup
history work is retained; its historical report is available in Git at
`423e086:docs/PROGRESS.md`. The root UI uses an external Hub, while integrated
UI calls the same board's Hub directly. Their setup guides must not be mixed.

## Remaining work, in order

1. Verify a real cup on `b923fa91`, then repeat alternating-handle tests beyond
   the single successful cup in each direction on the earlier `c88791b2` build.
2. Scale-off weight changes retained at scale power-on and App/device display
   agreement. The completed off/on/off cycle did not issue preset commands.
3. Network/Cloud interruptions, hardware stale/unknown transitions,
   deadline/cancellation timing, physical touch latency and extended runtime
   with touch, BLE, TLS, log parsing and feedback active together.
4. A reliable live extraction-start signal. Current timer checks do not prove a
   physical extraction lock: wait for confirmed target weight before brewing.
5. Dedicated integrated-headless, E80 and other-board acceptance. AMOLED board/UI
   work remains separate; this 1.69-inch scale result does not certify it.
6. Physical schedule edit/readback and remaining App/Widget/Watch/Siri
   matrices. Their application projects are maintained separately.
7. Production security/installation gates and upstream recipe-relative
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
