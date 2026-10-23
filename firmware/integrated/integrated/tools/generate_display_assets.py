from pathlib import Path
import argparse,io,re,subprocess,shutil
from PIL import Image
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
parser=argparse.ArgumentParser(description='Regenerate the embedded CJK fonts, Inter numerals and Tabler icons.')
parser.add_argument('source_directory',type=Path,help='Cache containing the original fonts and tabler/package/icons')
parser.add_argument('--new-cjk-only',action='store_true',help='Regenerate only the additional CJK sizes; preserve legacy fonts, numerals and icons')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]; assets=args.source_directory.resolve(); out=root/'display'
for name in ['InterVariable.ttf','NotoSansCJKsc-Regular.otf','NotoSansCJKsc-Bold.otf']:
    if not (assets/name).is_file(): parser.error('Missing source: '+name)
font=instantiateVariableFont(TTFont(assets/'InterVariable.ttf'),{'wght':700,'opsz':24},inplace=False)
font.save(assets/'InterDisplay-Bold.ttf')
font=instantiateVariableFont(TTFont(assets/'InterVariable.ttf'),{'wght':800,'opsz':24},inplace=False)
font.save(assets/'InterDisplay-ExtraBold.ttf')
font=instantiateVariableFont(TTFont(assets/'InterVariable.ttf'),{'wght':750,'opsz':18},inplace=False)
font.save(assets/'InterBattery-Semibold.ttf')
chars=''.join(sorted(set(c for path in [out/'gbs_ui.c',out/'gbs_ui_amoled.inc',out/'gbs_ui_amoled_details.inc'] for c in path.read_text() if ord(c)>127)))
(assets/'symbols.txt').write_text(chars)
original_cjk={(10,False),(12,False),(14,False),(12,True),(14,True),(16,True),(20,True)}
generated_fonts=[]
for size, bold in [(10,False),(12,False),(14,False),(12,True),(14,True),(16,True),(20,True),(18,False),(20,False),(24,False),(24,True),(28,True),(34,True),(19,False),(23,False),(25,True),(32,False)]:
    if args.new_cjk_only and (size,bold) in original_cjk: continue
    compression=['--no-compress'] if (size,bold) in original_cjk else []
    target=out/'fonts'/(f'gbs_font_{size}_bold.c' if bold else f'gbs_font_{size}.c')
    generated_fonts.append(target)
    subprocess.run(['npx','--yes','lv_font_conv@1.5.3','--font',str(assets/('NotoSansCJKsc-Bold.otf' if bold else 'NotoSansCJKsc-Regular.otf')),'--range','0x20-0x7e','--symbols',chars,'--size',str(size),'--bpp','4','--format','lvgl',*compression,'--lv-include','lvgl.h','--lv-fallback','lv_font_montserrat_14','-o',str(target)],check=True)
if args.new_cjk_only:
    for path in generated_fonts:
        text=re.sub(r'^ \* Opts:.*\n','',path.read_text(),flags=re.M)
        path.write_text(text.rstrip()+'\n')
    print('Generated',len(generated_fonts),'losslessly compressed CJK sizes; legacy fonts and numerals unchanged')
    raise SystemExit(0)
subprocess.run(['npx','--yes','lv_font_conv@1.5.3','--font',str(assets/'InterBattery-Semibold.ttf'),'--symbols','0123456789%—','--size','18','--bpp','4','--format','lvgl','--no-compress','--lv-include','lvgl.h','-o',str(out/'fonts/gbs_battery_18.c')],check=True)
amoled_numbers={27:(650,27),30:(700,30),52:(650,32),55:(700,32),70:(700,32),76:(700,32),108:(700,32)}
for size in [16,18,20,24,27,28,30,32,34,36,40,48,52,55,56,60,64,68,70,76,108]:
    source=assets/('InterDisplay-ExtraBold.ttf' if size in [28,32,34,36,40] else 'InterDisplay-Bold.ttf')
    if size in amoled_numbers:
        weight,optical=amoled_numbers[size]
        source=assets/f'InterAMOLED-{weight}-{optical}.ttf'
        instantiateVariableFont(TTFont(assets/'InterVariable.ttf'),{'wght':weight,'opsz':optical},inplace=False).save(source)
    subprocess.run(['npx','--yes','lv_font_conv@1.5.3','--font',str(source),'--symbols','0123456789.:°%sg—+- /C×','--size',str(size),'--bpp','4','--format','lvgl','--no-compress','--lv-include','lvgl.h','-o',str(out/'fonts'/f'gbs_number_{size}.c')],check=True)
for size in [24,27,36]:
    source=assets/f'InterUnit-{size}.ttf'
    instantiateVariableFont(TTFont(assets/'InterVariable.ttf'),{'wght':500,'opsz':min(size,32)},inplace=False).save(source)
    subprocess.run(['npx','--yes','lv_font_conv@1.5.3','--font',str(source),'--symbols','g s °C %','--size',str(size),'--bpp','4','--format','lvgl','--no-compress','--lv-include','lvgl.h','-o',str(out/'fonts'/f'gbs_unit_{size}.c')],check=True)
import cairosvg
names=['droplet','cloud-rain','link','settings','chevron-left','chevron-right','weight','check','alert-circle','help-circle','coffee','temperature','device-desktop','list-details','wifi','bluetooth','cloud','blender','circle-dotted','plus','minus','sun','power','clock','calendar','circle-check-filled','volume','eye','grinder','steam-status','refresh','file-search','cloud-off','cloud-check','arrow-right','hand-stop','alert-triangle','history','scale-frame']
header=['#pragma once','#include "lvgl.h"','typedef enum {']
for n in names: header.append('    GBS_ICON_'+n.replace('-','_').upper()+',')
header+=['    GBS_ICON_COUNT','} gbs_icon_t;','extern const lv_img_dsc_t *const gbs_icons[GBS_ICON_COUNT];']
(out/'icons/gbs_icons.h').write_text('\n'.join(header)+'\n')
source=['// Tabler Icons 3.35.0 (MIT) and the approved scale artwork, 8-bit alpha masks.','#include "gbs_icons.h"']
for n in names:
    local=out/'icons'/(n+'.svg')
    path=local if n in ['grinder','steam-status','refresh','file-search','cloud-off','cloud-check','arrow-right','hand-stop','alert-triangle','history'] else assets/'tabler/package/icons'/('filled' if n.endswith('-filled') else 'outline')/(n.replace('-filled','')+'.svg')
    if n=='scale-frame':
        width,height=172,126
        im=Image.open(out/'icons/scale-frame.png').convert('RGBA').resize((width,height),Image.Resampling.LANCZOS)
    else:
        assert path.exists()
        if path.resolve()!=local.resolve(): shutil.copy2(path,local)
        svg=path.read_text().replace('currentColor','#ffffff')
        width=height=96
        im=Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode(),output_width=384,output_height=384))).convert('RGBA').resize((width,height),Image.Resampling.LANCZOS)
    data=list(im.getchannel('A').tobytes()); assert max(data)>0
    name=n.replace('-','_')
    source+=['static const uint8_t '+name+'_data[]={']
    for i in range(0,len(data),36): source.append(','.join(str(x) for x in data[i:i+36])+',')
    source+=['};','static const lv_img_dsc_t '+name+'={.header={.cf=LV_IMG_CF_ALPHA_8BIT,.w='+str(width)+',.h='+str(height)+'},.data_size=sizeof('+name+'_data),.data='+name+'_data};']
source+=['const lv_img_dsc_t *const gbs_icons[GBS_ICON_COUNT]={'+','.join('&'+n.replace('-','_') for n in names)+'};']
(out/'icons/gbs_icons.c').write_text('\n'.join(source)+'\n')
print('Generated bold CJK, Inter numerals, and',len(names),'licensed icons')

for path in (out/'fonts').glob('*.c'):
 text=path.read_text()
 text=re.sub(r'^ \* Opts:.*\n','',text,flags=re.M)
 path.write_text(text.rstrip()+'\n')
