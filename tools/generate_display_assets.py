from pathlib import Path
import argparse,io,re,subprocess,shutil
from PIL import Image
import cairosvg
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
parser=argparse.ArgumentParser(description='Regenerate the embedded CJK fonts, Inter numerals and Tabler icons.')
parser.add_argument('source_directory',type=Path,help='Cache containing the original fonts and tabler/package/icons')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]; assets=args.source_directory.resolve(); out=root/'display'
for name in ['InterVariable.ttf','NotoSansCJKsc-Regular.otf','NotoSansCJKsc-Bold.otf']:
    if not (assets/name).is_file(): parser.error('Missing source: '+name)
font=instantiateVariableFont(TTFont(assets/'InterVariable.ttf'),{'wght':700,'opsz':24},inplace=False)
font.save(assets/'InterDisplay-Bold.ttf')
font=instantiateVariableFont(TTFont(assets/'InterVariable.ttf'),{'wght':800,'opsz':24},inplace=False)
font.save(assets/'InterDisplay-ExtraBold.ttf')
chars=''.join(sorted(set(c for c in (out/'gbs_ui.c').read_text() if ord(c)>127)))
(assets/'symbols.txt').write_text(chars)
for size, bold in [(10,False),(12,False),(14,False),(12,True),(14,True),(16,True),(20,True)]:
    subprocess.run(['npx','--yes','lv_font_conv@1.5.3','--font',str(assets/('NotoSansCJKsc-Bold.otf' if bold else 'NotoSansCJKsc-Regular.otf')),'--range','0x20-0x7e','--symbols',chars,'--size',str(size),'--bpp','4','--format','lvgl','--no-compress','--lv-include','lvgl.h','--lv-fallback','lv_font_montserrat_14','-o',str(out/'fonts'/(f'gbs_font_{size}_bold.c' if bold else f'gbs_font_{size}.c'))],check=True)
for size in [16,20,24,28,32,34,36,40]:
    subprocess.run(['npx','--yes','lv_font_conv@1.5.3','--font',str(assets/('InterDisplay-ExtraBold.ttf' if size>=28 else 'InterDisplay-Bold.ttf')),'--symbols','0123456789.:°%sg—+- /C','--size',str(size),'--bpp','4','--format','lvgl','--no-compress','--lv-include','lvgl.h','-o',str(out/'fonts'/f'gbs_number_{size}.c')],check=True)
names=['droplet','cloud-rain','link','settings','chevron-left','chevron-right','weight','check','alert-circle','help-circle','coffee','temperature','device-desktop','list-details','wifi','bluetooth','cloud','blender','circle-dotted','plus','minus','sun','power','clock','calendar','circle-check-filled']
header=['#pragma once','#include "lvgl.h"','typedef enum {']
for n in names: header.append('    GBS_ICON_'+n.replace('-','_').upper()+',')
header+=['    GBS_ICON_COUNT','} gbs_icon_t;','extern const lv_img_dsc_t *const gbs_icons[GBS_ICON_COUNT];']
(out/'icons/gbs_icons.h').write_text('\n'.join(header)+'\n')
source=['// Rasterized from Tabler Icons 3.35.0 (MIT), RGB565 plus alpha.','#include "gbs_icons.h"']
for n in names:
    path=assets/'tabler/package/icons'/('filled' if n.endswith('-filled') else 'outline')/(n.replace('-filled','')+'.svg'); assert path.exists()
    shutil.copy2(path,out/'icons'/(n+'.svg'))
    svg=path.read_text().replace('currentColor','#ffffff')
    size=72 if n=='check' else 48
    im=Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode(),output_width=192,output_height=192))).convert('RGBA').resize((size,size),Image.Resampling.LANCZOS)
    alpha=im.getchannel('A').tobytes(); assert max(alpha)>0
    data=[b for a in alpha for b in (255,255,a)]
    name=n.replace('-','_')
    source+=['static const uint8_t '+name+'_data[]={']
    for i in range(0,len(data),36): source.append(','.join(str(x) for x in data[i:i+36])+',')
    source+=['};','static const lv_img_dsc_t '+name+'={.header={.cf=LV_IMG_CF_TRUE_COLOR_ALPHA,.w='+str(size)+',.h='+str(size)+'},.data_size=sizeof('+name+'_data),.data='+name+'_data};']
source+=['const lv_img_dsc_t *const gbs_icons[GBS_ICON_COUNT]={'+','.join('&'+n.replace('-','_') for n in names)+'};']
(out/'icons/gbs_icons.c').write_text('\n'.join(source)+'\n')
print('Generated bold CJK, Inter numerals, and',len(names),'licensed icons')

for path in (out/'fonts').glob('*.c'):
 text=path.read_text()
 text=re.sub(r'^ \* Opts:.*\n','',text,flags=re.M)
 path.write_text(text.rstrip()+'\n')
