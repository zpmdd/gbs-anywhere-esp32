# Development progress

Updated 2026-10-22. The complete integrated firmware is published in this
repository; the original lightweight and external-Hub display builds remain
available. This update records both directions of real-cup recipe following and
the scale connection-cache defect. All variants remain experimental. Source
maintenance and local validation do not access or update devices.

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
  after 60 seconds. Missing fields remain unknown. No updated firmware or scale
  power-on/off hardware acceptance is claimed.

## Build and validation boundaries

| Surface | Evidence and status |
| --- | --- |
| Integrated touchscreen source | Imported runtime from `050790dc`; source build `c88791b2` has one successful cup in each direction. Public rebuilding or the cache correction changes its binary identity and needs separate hardware acceptance. |
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

1. Verify the scale connection-cache correction on hardware, then repeat
   alternating-handle tests beyond the single successful cup in each direction.
2. Network/Cloud interruptions, deadline/cancellation timing and extended runtime
   with touch, BLE, TLS, log parsing and feedback active together.
3. A reliable live extraction-start signal. Current timer checks do not prove a
   physical extraction lock: wait for confirmed target weight before brewing.
4. Dedicated integrated-headless hardware and E80/other-board acceptance.
5. Scale-off weight selection retained at power-on, bidirectional client display
   agreement, physical schedule edit/readback and remaining App/Widget/Watch/Siri
   matrices. Their application projects are maintained separately.
6. Production security/installation gates and upstream recipe-relative
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
