#!/usr/bin/env python3
"""Host-check the board's actual orientation functions without ESP-IDF hardware."""
from pathlib import Path
import re
import subprocess
import tempfile


display = Path(__file__).resolve().parents[1] / "display"
source = (display / "gbs_board_mosaico.c").read_text()


def function(name):
    match = re.search(r"^(?:static )?(?:gbs_orientation_t|esp_err_t) " + name +
                      r"\([^)]*\)\s*\{", source, re.M)
    assert match, name
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


# Cover both startup and later rotation: the first frame must be calibrated too.
assert "ESP_GOTO_ON_ERROR(set_panel_orientation(rotation.current)" in source
assert "if (set_panel_orientation(next)==ESP_OK)" in source

test = r'''
#include "gbs_device_policy.h"
#include <assert.h>
typedef int esp_err_t;
static gbs_rotation_policy_t rotation;
static uint8_t command_reg,command_value;
static int command_result;
static esp_err_t command(uint8_t reg,uint8_t value)
{
    command_reg=reg; command_value=value; return command_result;
}
'''
test += "\n".join(function(name) for name in
                  ("render_orientation", "set_panel_orientation",
                   "gbs_board_amoled216_orientation"))
test += r'''
int main(void)
{
    const uint8_t expected_madctl[]={0x60,0x00,0xA0,0xC0};
    const gbs_orientation_t expected_render[]={3,0,1,2};
    const uint16_t points[][2]={{0,0},{479,0},{479,479},{0,479},{123,317}};
    gbs_rotation_policy_init(&rotation);
    assert(rotation.current==GBS_ORIENTATION_USB_RIGHT);
    assert(set_panel_orientation(rotation.current)==0 && command_value==0x60);
    for(unsigned physical=0;physical<4;physical++) {
        rotation.current=(gbs_orientation_t)physical;
        gbs_orientation_t render=gbs_board_amoled216_orientation();
        assert(render==expected_render[physical]);
        assert(rotation.current==(gbs_orientation_t)physical);
        assert(set_panel_orientation(rotation.current)==0);
        assert(command_reg==0x36 && command_value==expected_madctl[physical]);
        for(unsigned p=0;p<sizeof(points)/sizeof(points[0]);p++) {
            uint16_t x=points[p][0],y=points[p][1],raw_x=x,raw_y=y,sx,sy;
            /* CO5300 address transform, independent of the touch helper. */
            switch(command_value) {
            case 0x00: break;
            case 0xA0: raw_x=y; raw_y=479-x; break;
            case 0xC0: raw_x=479-x; raw_y=479-y; break;
            case 0x60: raw_x=479-y; raw_y=x; break;
            default: assert(0);
            }
            assert(gbs_device_map_touch(render,raw_x,raw_y,&sx,&sy));
            assert(sx==x && sy==y);
        }
    }
    command_result=-1;
    assert(set_panel_orientation(GBS_ORIENTATION_USB_RIGHT)==-1);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="gbs-mosaico-orientation-") as directory:
    root = Path(directory)
    (root / "test.c").write_text(test)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", str(display),
                    str(root / "test.c"), str(display / "gbs_device_policy.c"),
                    "-lm", "-o", str(root / "test")], check=True)
    subprocess.run([str(root / "test")], check=True)
print("Mosaico orientation: startup, four directions and four corners passed")
