"""Generate the approved AMOLED flow assets without changing legacy icons."""

import argparse
import io
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from PIL import Image


FLOW_ICONS = [
    'grinder', 'machine', 'scale', 'battery', 'cloud', 'settings', 'bluetooth',
    'done', 'wait-grinder', 'file', 'upload', 'clock', 'refresh', 'alert',
    'minus', 'drop', 'steam', 'edit-minus', 'edit-plus', 'node-ring',
    'chevron-right', 'back',
]
CJK_FONTS = [
    (15, 'regular'), (16, 'regular'), (17, 'regular'), (18, 'regular'),
    (19, 'regular'), (20, 'regular'), (23, 'regular'), (24, 'regular'), (32, 'regular'),
    (16, 'semibold'), (23, 'semibold'), (28, 'semibold'), (30, 'semibold'),
    (24, 'bold'), (25, 'bold'), (28, 'bold'), (34, 'bold'),
]
NUMBER_FONTS = [(45, 800), (31, 800), (29, 750), (72, 800)]
NUMBER_METRICS = {45: (45, 10), 31: (31, 7), 29: (29, 6), 72: (70, 15)}
NUMBER_SYMBOLS = '0123456789.:°%sg—+- /C×'
# These fonts are only used by the AMOLED profile; shared LCD assets stay unchanged.
DETAIL_NUMBER_SIZES = [18, 27, 30, 48, 52, 55, 56, 60, 64, 68, 70, 76, 108]
# Stronger outlines need one extra edge row at these small sizes.
METRIC_OVERRIDES = {'gbs_font_18': (22, 5), 'gbs_battery_18': (15, 1)}


def generate_icons(out):
    """Append native-size alpha masks; preserve every legacy descriptor and index."""
    import cairosvg

    directory = out / 'icons'
    source = ['// Generated from the approved flow SVGs; see FLOW-SOURCES.md.']
    pointers = []
    for name in FLOW_ICONS:
        svg = (directory / f'flow-{name}.svg').read_text()
        root = ET.fromstring(svg)
        assert not root.findall('.//{http://www.w3.org/2000/svg}text')
        width, height = int(root.get('width')), int(root.get('height'))
        png = cairosvg.svg2png(bytestring=svg.replace('currentColor', '#fff').encode())
        alpha = Image.open(io.BytesIO(png)).getchannel('A')
        assert alpha.size == (width, height)
        data = alpha.tobytes()
        assert len(data) == width * height and max(data) > 0
        symbol = 'flow_' + name.replace('-', '_')
        source.append(f'static const uint8_t {symbol}_data[]={{')
        source.extend(','.join(str(x) for x in data[i:i+32]) + ',' for i in range(0, len(data), 32))
        source.extend([
            '};',
            f'static const lv_img_dsc_t {symbol}={{.header={{.cf=LV_IMG_CF_ALPHA_8BIT,.w={width},.h={height}}},'
            f'.data_size=sizeof({symbol}_data),.data={symbol}_data}};',
        ])
        pointers.append('&' + symbol)
    (directory / 'gbs_flow_icons.inc').write_text('\n'.join(source) + '\n')
    header = directory / 'gbs_icons.h'
    text = re.sub(r'^    GBS_ICON_FLOW_[A-Z_]+,\n', '', header.read_text(), flags=re.M)
    enums = ''.join('    GBS_ICON_FLOW_' + name.replace('-', '_').upper() + ',\n' for name in FLOW_ICONS)
    assert text.count('    GBS_ICON_COUNT') == 1
    header.write_text(text.replace('    GBS_ICON_COUNT', enums + '    GBS_ICON_COUNT'))
    path = directory / 'gbs_icons.c'
    text = path.read_text().replace('#include "gbs_flow_icons.inc"\n', '')
    match = re.search(r'const lv_img_dsc_t \*const gbs_icons\[GBS_ICON_COUNT\]=\{([^}]+)\};', text)
    assert match, 'Missing legacy icon table'
    legacy = [p for p in match[1].split(',') if not p.startswith('&flow_')]
    table = '#include "gbs_flow_icons.inc"\nconst lv_img_dsc_t *const gbs_icons[GBS_ICON_COUNT]={'
    table += ','.join(legacy + pointers) + '};'
    path.write_text(text[:match.start()] + table + text[match.end():])


def instance(source, target, symbols, weight, optical=None):
    font = TTFont(source)
    missing = set(map(ord, symbols)) - set(font.getBestCmap())
    assert not missing, f'{source.name} lacks {sorted(missing)}'
    selection = subset.Subsetter()
    selection.populate(text=symbols)
    selection.subset(font)
    axes = {'wght': weight}
    if optical is not None:
        axes['opsz'] = optical
    frozen = instantiateVariableFont(font, axes, inplace=True)
    assert 'fvar' not in frozen, 'Every variable axis must be frozen'
    assert frozen['OS/2'].usWeightClass == weight
    frozen.save(target)


def convert(source, target, size, symbols, cjk=False):
    old = target.read_text() if target.exists() else ''
    metrics = [re.search(r'\.' + key + r' = (\d+)', old) for key in ('line_height', 'base_line')]
    args = ['npx', '--yes', 'lv_font_conv@1.5.3', '--font', str(source),
            '--symbols', symbols, '--size', str(size), '--bpp', '4', '--format',
            'lvgl', '--lv-include', 'lvgl.h', '-o', str(target)]
    if cjk:
        args += ['--lv-fallback', 'lv_font_montserrat_14']
    else:
        args += ['--no-compress']
    subprocess.run(args, check=True)
    text = re.sub(r'^ \* Opts:.*\n', '', target.read_text(), flags=re.M)
    for key, value in zip(('line_height', 'base_line'), metrics):
        if value:
            text = re.sub(r'\.' + key + r' = \d+', '.' + key + ' = ' + value[1], text)
    if target.stem in METRIC_OVERRIDES:
        for key, value in zip(('line_height', 'base_line'), METRIC_OVERRIDES[target.stem]):
            text = re.sub(r'\.' + key + r' = \d+', '.' + key + ' = ' + str(value), text)
    target.write_text(text.rstrip() + '\n')


def semibold_subsets(out):
    """Collect only text rendered by the four dedicated semibold fonts."""
    source = (out / 'gbs_ui_amoled.inc').read_text()
    shared = (out / 'gbs_ui.c').read_text()
    ascii_symbols = set(chr(c) for c in range(0x20, 0x7f))
    strings = lambda text: re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', text)
    home = source.split('static void amoled_home(void)', 1)[1]
    nodes = re.search(r'\bnames\[\]\s*=\s*\{([^}]+)\}', home)
    assert nodes, 'Missing home node labels'
    parameters = re.search(r'\bparameters\[\]\s*=\s*\{(.*?)\n\};', shared, re.S)
    assert parameters, 'Missing shared editor parameter table'
    parameter_strings = strings(parameters[1])
    assert len(parameter_strings) % 2 == 0
    units = ''.join(parameter_strings[1::2])
    headlines = source.split('const char *title="等待磨粉"', 1)[1].split(
        'const lv_font_t *headline=', 1)[0]
    headlines = 'title="等待磨粉"' + headlines
    assignments = re.findall(r'\btitle\s*=\s*(.*?)(?=;|,\s*\*subtitle\s*=)', headlines, re.S)
    titles = [text for expression in assignments for text in strings(expression)]
    assert titles and '等待磨粉' in titles and '设定结果未确认' in titles
    subsets = {
        16: ascii_symbols | set(''.join(strings(nodes[1]))),
        23: ascii_symbols | set('应用并记住已设定' + units),
        28: ascii_symbols | set(''.join(titles)),
        30: ascii_symbols | set(''.join(titles)),
    }
    return {size: ''.join(sorted(symbols)) for size, symbols in subsets.items()}


def generate_numbers(out, assets, include_battery=True):
    declarations = []
    with tempfile.TemporaryDirectory(prefix='gbs-flow-numbers-') as temporary:
        for size, weight in NUMBER_FONTS + ([(13, 750)] if include_battery else []):
            name = f'gbs_number_{size}' if size != 13 else 'gbs_battery_13'
            font = Path(temporary) / (name + '.ttf')
            optical = min(32, max(14, size))
            instance(assets / 'InterVariable.ttf', font, NUMBER_SYMBOLS, weight, optical)
            target = out / 'fonts' / (name + '.c')
            convert(font, target, size, NUMBER_SYMBOLS)
            if size in NUMBER_METRICS:
                height, baseline = NUMBER_METRICS[size]
                text = re.sub(r'\.line_height = \d+', f'.line_height = {height}', target.read_text())
                text = re.sub(r'\.base_line = \d+', f'.base_line = {baseline}', text)
                target.write_text(f'/* Inter Variable frozen at wght={weight}, opsz={optical}; size {size}. */\n' + text)
            declarations.append(name)
    return declarations


def generate_detail_numbers(out, assets):
    declarations = []
    specs = [(f'gbs_number_{size}', size, 750 if size <= 30 else 800, NUMBER_SYMBOLS)
             for size in DETAIL_NUMBER_SIZES]
    specs += [(f'gbs_amoled_number_{size}', size, 750, NUMBER_SYMBOLS) for size in (20, 24)]
    specs += [(f'gbs_unit_{size}', size, 650, 'g s °C %') for size in (24, 27, 36)]
    specs += [('gbs_battery_18', 18, 800, '0123456789%—')]
    with tempfile.TemporaryDirectory(prefix='gbs-detail-numbers-') as temporary:
        for name, size, weight, symbols in specs:
            font = Path(temporary) / (name + '.ttf')
            optical = min(32, max(14, size))
            instance(assets / 'InterVariable.ttf', font, symbols, weight, optical)
            target = out / 'fonts' / (name + '.c')
            if name.startswith('gbs_amoled_') and not target.exists():
                target.write_text((out / 'fonts' / f'gbs_number_{size}.c').read_text())
            convert(font, target, size, symbols)
            target.write_text(f'/* Inter Variable frozen at wght={weight}, opsz={optical}; size {size}. */\n' + target.read_text())
            declarations.append(name)
    return declarations


def generate_fonts(out, assets, semibold_only=False, cjk_only=False):
    directory = out / 'fonts'
    ui_sources = ''.join((out / filename).read_text() for filename in
                         ('gbs_ui.c', 'gbs_ui_amoled.inc', 'gbs_ui_amoled_details.inc'))
    symbols = ''.join(sorted(set((directory / 'flow-symbols.txt').read_text().strip())
                             | {c for c in ui_sources if ord(c) > 127}
                             | set(chr(c) for c in range(0x20, 0x7f))))
    required = ['NotoSansSC-VF.ttf', 'InterVariable.ttf']
    for filename in required:
        if not (assets / filename).is_file():
            raise FileNotFoundError(f'Missing font source: {assets / filename}')
    declarations = []
    narrow = semibold_subsets(out)
    with tempfile.TemporaryDirectory(prefix='gbs-flow-fonts-') as temporary:
        temp = Path(temporary)
        all_symbols = set(symbols)
        for size, weight in CJK_FONTS:
            suffix = '' if weight == 'regular' else '_' + weight
            target = directory / f'gbs_font_{size}{suffix}.c'
            if target.exists():
                all_symbols |= {chr(int(c, 16)) for c in re.findall(r'U\+([0-9A-Fa-f]+)', target.read_text())}
        instances = {}
        for weight in (650, 750):
            instances[weight] = temp / f'NotoFlow-{weight}.ttf'
            instance(assets / 'NotoSansSC-VF.ttf', instances[weight], ''.join(sorted(all_symbols)), weight)
        for size, weight in CJK_FONTS:
            if semibold_only and weight != 'semibold':
                continue
            suffix = '' if weight == 'regular' else '_' + weight
            name = f'gbs_font_{size}{suffix}'
            target = directory / (name + '.c')
            selected = narrow[size] if weight == 'semibold' else symbols
            if weight != 'semibold' and target.exists():
                legacy = {chr(int(c, 16)) for c in re.findall(r'U\+([0-9A-Fa-f]+)', target.read_text())}
                selected = ''.join(sorted(set(symbols) | legacy))
            actual_weight = 750 if weight != 'regular' or size == 32 else 650
            font = instances[actual_weight]
            cmap = TTFont(font).getBestCmap()
            assert set(map(ord, selected)) <= set(cmap), f'Missing glyph in {name}'
            convert(font, target, size, selected, cjk=True)
            target.write_text(f'/* Noto Sans SC Variable frozen at wght={actual_weight}; size {size}. */\n' + target.read_text())
            emitted = {int(c, 16) for c in re.findall(r'U\+([0-9A-Fa-f]+)', target.read_text())}
            assert set(map(ord, selected)) <= emitted, f'Incomplete flow font: {name}'
            declarations.append(name)
        if semibold_only or cjk_only:
            return
    declarations += generate_numbers(out, assets)
    declarations += generate_detail_numbers(out, assets)
    header = ['#pragma once', '#include "lvgl.h"']
    header += [f'LV_FONT_DECLARE({name});' for name in declarations]
    (directory / 'gbs_flow_fonts.h').write_text('\n'.join(header) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source_directory', type=Path)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--semibold-only', action='store_true', help='Only refresh the four scoped semibold fonts')
    selection.add_argument('--numbers-only', action='store_true', help='Only refresh the four main Inter numeral fonts')
    args = parser.parse_args()
    output = Path(__file__).resolve().parents[1] / 'display'
    if args.numbers_only:
        generate_numbers(output, args.source_directory.resolve(), include_battery=False)
    else:
        generate_fonts(output, args.source_directory.resolve(), args.semibold_only)
    if not args.semibold_only and not args.numbers_only:
        generate_icons(output)
    print('Generated main numeral fonts.' if args.numbers_only else
          'Generated scoped semibold fonts.' if args.semibold_only else
          'Generated flow fonts and native-size alpha icons; legacy icon data preserved.')
