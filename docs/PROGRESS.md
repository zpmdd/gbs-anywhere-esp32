# Development progress

Updated 2026-10-22. Current work publishes the complete integrated firmware in
this repository; the original lightweight and external-Hub display builds remain
available. All variants remain experimental. No device is accessed or updated
as part of this source publication.

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
  confirmed touch behavior. See the [exact-build report](RECIPE_FOLLOW_ACCEPTANCE.md).

## Build and validation boundaries

| Surface | Evidence and status |
| --- | --- |
| Integrated touchscreen source | Imported runtime from `050790dc`; source build `c88791b2` has the one-cup evidence above. Public rebuilding changes its binary identity. |
| Integrated headless | Shared Hub/grinder core with no display tasks. Dedicated integrated-headless hardware acceptance remains open. |
| Root companion and external-Hub preview | Existing source/features retained; job/coffee parser compatibility fixes synchronized. Their older hardware evidence does not certify the integrated build. |
| Host regressions | Both offline entry points passed locally: synthetic protocol/worker/UI and Hub/recipe/transaction checks, with ASan/UBSan and no real devices. |
| Local builds and simulators | All four profiles built: root headless/touchscreen and integrated headless/touchscreen. Both native simulator self-tests passed. Imported runtime C/headers match the archived source. |
| Initial provisioning | Imported identity starts in monitor mode. Actual NVS generation is tested at both touchscreen/headless sizes, with private file permissions and no credential echo. |
| CI | The [workflow](../.github/workflows/ci.yml) checks both applications and all four main firmware profiles. Its commit-specific result is the hosted evidence; builds do not establish physical acceptance. |
| Production release/security | Still experimental. No general firmware release, armed eFuse tool or security-gate certification is part of this publication. |

The earlier simulator drag, daily controls, schedule editing and multi-cup
history work is retained; its historical report is available in Git at
`423e086:docs/PROGRESS.md`. The root UI uses an external Hub, while integrated
UI calls the same board's Hub directly. Their setup guides must not be mixed.

## Remaining work, in order

1. Reverse automatic following to a 20 g recipe and alternating handles.
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

The integrated import keeps the deployed source layout and attribution, plus
only the public build/test/provisioning adaptations. See its
[source boundary](../firmware/integrated/UPSTREAM.md). No private configuration,
certificates, device identities, raw logs or firmware binaries are published.
