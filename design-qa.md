# Native touchscreen design QA

Date: 2026-10-17. Scope: visual alignment of the selected V3 design with the
shared 240 x 280 LVGL renderer. This is local visual QA, not physical hardware
or live Hub acceptance.

## Evidence and normalization

- Source truth: `.private/integrated-touchscreen-selected-v3-20261017/`, the nine
  numbered PNG boards from `01-home-ready-and-active.png` through
  `09-display-and-link-mode.png`.
- Source dimensions: 1586 x 992 for most boards; 1585 x 992 for board 03;
  1568 x 1003 for boards 02 and 04. Each board contains two screens.
- Implementation: `.private/native-touchscreen-20261017/final/*.png`, exported
  from the compiled native LVGL framebuffer, using synthetic values.
- Viewport and implementation pixels: 240 x 280, density 1. No CSS viewport or
  browser deviceScaleFactor applies. The Mac window enlarges the same pixels.
- Normalization: crop each source screen out of its surrounding presentation
  board and resize that content to 240 x 280 with Lanczos. Implementation pixels
  remain unscaled. Exact source bounds are recorded in
  `.private/native-touchscreen-20261017/comparison-normalization.json`.
- Full-view paired evidence: `comparison-final-1.png`, `comparison-final-2.png`
  and `comparison-final-3.png` in `.private/native-touchscreen-20261017/`.
  Each pair contains reference and actual pixels in the same image.
- Focused paired evidence: `comparison-focused.png` in that directory, showing
  the home dials and the central editor at the same 2x nearest-neighbor scale.
  This was used to check numeral weight, target-label size and icon clarity.
- User-facing overview: `final-overview.png`; complete paired review:
  `review.zh-CN.md` in the same directory.

The 18 paired states cover home ready/active/offline, waiting/success/canceling/
unknown delivery, records, weight editing/pending/confirmed, settings, machine
menu, boiler, steam, connections, display preferences and linkage mode.
Steam uses a synthetic 126-degree target with a 128-degree draft to represent
the enabled Apply state. Unknown delivery uses the current synthetic measured
answer. Unavailable real-machine fields are tested separately, not presented as
live telemetry.

## Findings and comparison history

1. **Initial capture: blocked.** [P1] Thin generic numerals, weak hierarchy,
   boxed gear, simplified cards and list styling materially differed from the
   selected design. Evidence: `before/`, `screens/` and `review-grid.png`.
   Fix: use separate Chinese UI/display numeral fonts, restore the two dials,
   four compact presets, unboxed header controls, flat divided settings rows,
   and the direct home linkage card.
2. **Pass 1: blocked.** [P2] RGB565 rounded the background toward cyan; result
   icons were blurry; metric captions crowded the footer; connection dots and
   machine-menu values were missing. Evidence: `comparison-pass1-1.png` and
   `comparison-pass1-2.png`. Fix: quantization-aware neutral palette, proper
   icon sizing, clearer metric spacing, status dots and aligned numeric values.
3. **Pass 2: blocked.** [P2] The success icon had an excessively thick border,
   active home cards omitted the target, pending editor feedback was too sparse,
   and steam lacked the intended toggle/level composition. Evidence:
   `comparison-pass2-1.png` through `comparison-pass2-3.png`. Fix: fine circle
   with the licensed check symbol, home target line, pending-state ring and
   confirmation panel, and capability-aware steam controls.
4. **Pass 3: blocked.** [P2] Disabled gold Apply controls had poor text contrast.
   Coordinate tests also exposed immediate-tap layout timing in the simulator.
   Evidence: `comparison-pass3-3.png` and native application observations.
   Fix: inactive controls use a dark surface with readable muted text. Native
   pointer tests update layout and process both edges of a rapid tap.
5. **Focused typography review: blocked.** [P2] Large numerals remained lighter
   than the source and dial targets occupied too much space. Fix: weight 800 for
   the large Inter numerals, 34 px home temperatures and 10 px secondary dial
   targets; keep 700 for small numerals and regular/bold Chinese UI text.
6. **Final comparison: passed.** The three final paired boards and updated
   focused image were inspected after regeneration. No actionable P0/P1/P2
   visual differences remain within the 18-state scope.

## Required fidelity surfaces

| Surface | Assessment |
| --- | --- |
| Fonts and typography | Noto Sans CJK SC regular/bold and Inter 700/800 reproduce the display/body hierarchy. Headers, primary values, units and secondary targets are distinct. No critical label wraps or collides with its control. The source provides no font metadata; exact generated glyph shapes are not reproducible font specifications. |
| Spacing and layout | Two home dials, four presets, the visible linkage card and direct settings entry follow the source. Settings use flat divider rows. Key actions retain 44 px hit regions while their visible faces are smaller. Result metrics and footer actions do not overlap. |
| Colors and tokens | Charcoal, warm ivory, champagne gold and muted green replace the earlier cool default appearance. RGB565 quantization is explicit. Warning/unknown states retain text as well as color. Disabled Apply text remains readable. |
| Assets and image quality | Tabler's licensed interface symbols are used consistently; large success graphics are crisp. There are no full-screen raster mockups, product illustrations or fabricated logos in the runtime UI. Rings, controls and dividers are native LVGL widgets. |
| Copy and content | The four preset amounts and temperature/linkage labels match the brief. Pending, confirmed and unknown states remain distinct. Canceling says that a task is ending; it does not incorrectly claim that every cancellation was caused by timeout. Missing machine data is explicitly unavailable. |

## Interaction and implementation checks

- Native self-test: all 23 page routes render; home-card and back coordinates
  survive eight rapid cycles; no background task steals navigation; duplicate
  submissions lock; a newly selected preset waits for confirmation; conflicts
  require returning/discarding; unavailable machine writes stay disabled.
- CUA native application observations: home to settings to machine to weight;
  two increment taps; Apply followed by pending and confirmed feedback; home
  linkage card to linkage detail to record and back. The window explicitly
  identifies synthetic data. `cua-record.png` retains a native navigation capture.
- Automation limitation: CUA sometimes delivered stale SDL button coordinates
  after synthetic numeric shortcut keys. A fresh launch allowed the pointer
  routes above. This is not claimed as a passed shortcut-plus-pointer test, nor
  as evidence about physical touchscreen input. Coordinate regression tests run
  directly against LVGL independently of that automation limitation.
- Host checks: `sh tools/check.sh`, including production worker regression tests
  and ASan/UBSan display-model tests, passed. Native `ctest` passed.
- ESP-IDF 5.5.4 built both 8 MB headless and 16 MB display profiles. No display
  or LVGL object symbols were linked into the headless ELF. No flash occurred.
- CI includes the native self-test and both firmware profiles. Commit-specific
  hosted results are linked from [development progress](docs/PROGRESS.md).

## Expected differences and follow-up polish

- [P3] The source's photographic glow/grain is not reproduced as a screen
  texture. Native RGB565 surfaces use subtle gradients; physical LCD calibration
  remains necessary before judging exact color.
- [P3] Standard icon silhouettes and a few glyph shapes differ slightly from the
  generated reference. Their meaning, color and placement remain consistent.
- [P3] Small source spacing variations were normalized into one shared native
  control system. Fixed idle-policy rows toggle the available policy rather than
  implying independently configurable timers that are not implemented.
- No physical screen, wake gesture, sustained network/memory load, machine
  transport, or real grinder/coffee acceptance was tested in this change.

## Hub integration follow-up, 2026-10-17

- The three interaction defects now have native regressions: a missing callback
  stops keeping the screen awake after 30 seconds, stale/rejected steam switches
  retain their confirmed state, and a cloud-error linkage dot uses warning color.
  A request ID also prevents an old result from confirming a newer operation.
- The new Hub client uses existing authenticated HTTPS v2 routes. The interface
  retains the approved typography, neutral RGB565 palette, two dials, four presets
  and direct linkage card. The connection row now says Hub and uses a link icon
  to reflect the actual transport. Steam adds compact pending/confirmed/unknown
  feedback; stale levels remain readable with the current selection outlined.
- Current exports: `.private/native-touchscreen-20261017/hub-integration/*.png`.
  All 18 normalized comparisons were regenerated as `comparison-hub-1.png`
  through `comparison-hub-3.png` using the existing crop coordinates.
  `hub-integration-overview.png` shows measured versus target values, confirmed
  weight, pending/confirmed steam, stale data and cloud failure. All data is
  synthetic; these are compiled LVGL pixels, not live-machine screenshots.
- CUA native pointer checks observed Home → Settings → Machine → Steam, then
  a switch submission that retained the old position while pending and changed
  only with confirmed feedback. This verifies the simulator's UI path; Hub
  protocol behavior is covered separately by the production C contract tests.
- Host ASan/UBSan tests, 11 provisioning tests, 8 serial-helper tests, native
  interaction tests and both firmware builds passed locally. No Hub/LVGL symbols
  are linked into the headless image. Real TLS, physical display, machine controls
  and sustained memory/network load remain untested. Hosted CI evidence is tracked
  separately from this local visual review in [development progress](docs/PROGRESS.md).

## Daily controls follow-up, 2026-10-18

- BBW, prebrew, power and smart-standby pages reuse the approved Noto/Inter
  typography, RGB565 palette, rings, compact headers and 44 px actions. The
  home/navigation design is unchanged; no bottom navigation was added.
- Exports: `.private/daily-controls-20261018/screens/`, with three six-screen
  review sheets in that directory's parent. The public
  [daily-controls overview](docs/images/daily-controls-preview.png) contains
  unscaled 240 x 280 framebuffer captures and labels, not concept artwork.
- Visual review corrected time-parameter icons, power-status spacing and disabled
  confirmation contrast. Pending numeric controls label the requested value and
  retain the current confirmed value separately. Confirmations and concurrent
  edits remain distinguishable from success.
- All 32 routes render. Native tests cover BBW modes/doses, prebrew precision and
  limits, explicit power/standby confirmations, duplicate locking, related-field
  conflicts, stale/unavailable controls and partial/unknown results. Visible-label
  bounds are checked on the new editor routes. A direct LVGL swipe reaches the
  offscreen power row without opening the row under the initial contact.
- CUA pointer checks observed BBW and prebrew edit → pending → confirmed with
  synthetic data. CUA drag-to-scroll still sometimes opened a row instead of
  scrolling after the simulator's motion events were preserved. Its cause is not
  established; that automation path remains unaccepted. This does not replace
  the separately required physical-touch validation.
- Production Hub contract tests with ASan/UBSan and both ESP-IDF profiles passed
  locally. New controls use one POST and verify the fresh readback, including
  fields that must be preserved. These checks do not validate a live Hub or the
  machine's physical response. No existing device was contacted or modified.

## Schedule/history and pointer follow-up, 2026-10-18

- Resolved the earlier CUA drag failure: AppKit delivered drag coordinates that
  SDL2 omitted, followed by an SDL release at an old coordinate. The simulator
  now reads its own window's native pointer events and delivers final movement
  before release. Native CUA reproduced the original gesture without opening a
  row; the following tap opened Schedule. This is simulator evidence only.
- Added list/detail/refresh, time/day/toggle draft editing, explicit confirmation,
  pending/confirmed/unknown/conflict states and multi-cup history. The overview
  at [schedule/history preview](docs/images/schedule-history-preview.png) uses
  native 240 x 280 framebuffer exports with synthetic data.
- Visual comparison found missing numeric-font spaces, slashes and `C`; the
  subset and generator now include them. Render-time glyph assertions prevent
  placeholder boxes from silently passing. The Noto/Inter fonts, colors, compact
  headers, 44 px actions and existing home/navigation layout are retained.
- All 38 routes render. Native regressions cover paging, refresh coalescing,
  missing metrics, empty/error/stale states, draft-only changes, confirmation,
  exact save feedback, concurrent changes, timeout, discard and subsequent taps.
- Final native CUA checks passed Schedule → Edit → Confirm → Pending → Confirmed
  and History → Next page → Missing-value detail. No real machine data or writes
  were used. The separate Hub parser, transaction, cache-race and coordinator
  tests cover the wire contract; synthetic UI success is not a live Hub claim.
- Both companion profiles and the separate Hub firmware build passed locally.
  Physical panel/touch, real TLS/vendor readback, timezone behavior and sustained
  resource use remain unverified. No current device was contacted or modified.

## Implementation checklist

- [x] Align typography, colors, layout and standard interface symbols.
- [x] Restore key home information and pending/confirmed/exception presentations.
- [x] Compare 18 states in normalized paired images, plus focused typography.
- [x] Run native interaction/model regressions and both firmware builds.
- [ ] Validate the exact display binary on authorized physical hardware.

final result: passed
