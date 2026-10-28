# Approved AMOLED flow fonts

All embedded font data is licensed under SIL OFL 1.1. The existing Noto and
Inter license texts are retained in `OFL.txt` and `Inter-OFL.txt`.

The design preview uses macOS system fonts, including PingFang SC. Firmware
uses Noto Sans SC/CJK SC for Chinese and Inter for numeric readouts. These
have different glyph shapes, advances, and baselines; a matching nominal size
does not imply pixel-identical Mac text. No macOS system font is embedded.

| Generated symbol | Source / weight |
| --- | --- |
| `gbs_font_15`, `16`, `17`, `18`, `19`, `20`, `23`, `24` | Noto Sans SC Variable, frozen at 650 |
| `gbs_font_16_semibold`, `23_semibold`, `28_semibold`, `30_semibold` | Noto Sans SC Variable, frozen at 750 |
| `gbs_font_24_bold`, `25_bold`, `28_bold`, `34_bold`, `gbs_font_32` | Noto Sans SC Variable, frozen at 750 |
| `gbs_number_45` | Inter Variable, 800, optical size 32 |
| `gbs_number_31` | Inter Variable, 800, optical size 31 |
| `gbs_number_29` | Inter Variable, 750, optical size 29 |
| `gbs_number_72` | Inter Variable, 800, optical size 32 |
| `gbs_number_18`, `27`, `30`, `gbs_amoled_number_20`, `24` | Inter Variable, 750, optical size matching nominal size |
| `gbs_number_48`, `52`, `55`, `56`, `60`, `64`, `68`, `70`, `76`, `108` | Inter Variable, 800, optical size 32 |
| `gbs_unit_24`, `27`, `36` | Inter Variable, 650, optical size capped at 32 |
| `gbs_battery_13` | Inter Variable, 750, optical size 14 |
| `gbs_battery_18` | Inter Variable, 800, optical size 18 |

The AMOLED weights were increased after physical-screen review. Each is
frozen from the variable source before conversion; the generator verifies
that no variable axes remain and the static weight class matches. Nominal
size and line-height/baseline pairs remain 45/10, 31/7, 29/6, and 70/15
respectively. Natural glyph advances are retained without horizontal scaling.
Other existing metrics are preserved, except the 18 px body font uses 22/5
and the 18 px battery font uses 15/1 to fit the heavier glyph edges.
No bitmap dilation or synthetic outline is applied. Existing symbol names
are retained for compatibility; `regular` and `semibold` names no longer
describe the calibrated weight. The shared LCD fonts are unchanged, including
the original 20/24 px numerals; the two `gbs_amoled_number_*` assets serve only
native AMOLED layouts. Shared 28/40 px numerals already use weight 800.

Body and existing bold CJK subsets include ASCII, all 98 non-ASCII
characters from the approved thirteen-screen design in `flow-symbols.txt`,
and the text in `gbs_ui_amoled.inc`, `gbs_ui_amoled_details.inc`, and `gbs_ui.c`,
including shared parameter titles, units, and status strings. They preserve
prior glyph coverage.

The four dedicated semibold subsets retain ASCII and only their own UI text
to keep the 4 MiB application partition usable:

- 16: the five home node names, collected from `amoled_home`, and `?`.
- 23: `应用`, `已设定`, and every unit from the shared parameter table.
- 28 and 30: every constant in the home `title` assignments, including
  conditional branches, collected automatically from the refresh logic.

The generator asserts source cmap coverage and emitted glyph coverage for
each subset. Add new consumers to these rules before reusing a scoped font.

## Original sources

- Noto Sans CJK SC Regular/Bold: version 2.004, copyright 2014-2021 Adobe;
  existing source cache and license.
- [Noto Sans SC Variable](https://github.com/notofonts/noto-cjk/blob/main/Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf):
  version 2.004, copyright 2014-2021 Adobe, Reserved Font Name `Source`;
  [upstream OFL](https://github.com/notofonts/noto-cjk/blob/main/Sans/LICENSE).
- Inter Variable: version 4.001 (`git-9221beed3`), copyright 2016 The Inter
  Project Authors; existing source cache and license.

Source SHA-256 values:

```text
2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b  NotoSansCJKsc-Regular.otf
b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a  NotoSansCJKsc-Bold.otf
d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964  NotoSansSC-VF.ttf
4989b125924991b90d05b2d16e0e388c48f7d5bb8b30539bbf9c755278d0ccaf  InterVariable.ttf
```

## Regeneration

From the integrated application directory, run:

```sh
python tools/generate_flow_assets.py /path/to/source-cache
```

Use `--semibold-only` to refresh only the four scoped semibold files without
rewriting regular, legacy, numeric, or icon assets.
Use `--numbers-only` to refresh only the four main numeral fonts without
rewriting the battery font, other fonts, or icons.

The source cache must contain `NotoSansSC-VF.ttf` and `InterVariable.ttf`.
The legacy full-asset generator additionally requires the original Noto OTFs;
it calls this generator last so a full rebuild retains the AMOLED weights.
The script
reuses the existing FontTools, Pillow, CairoSVG, and `lv_font_conv@1.5.3`
toolchain. On macOS, Cairo may require
`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib`. It generates compressed 4-bpp
CJK fonts, uncompressed 4-bpp numerals, and fixed-size alpha icons. No source
font is copied into firmware or fetched automatically.
