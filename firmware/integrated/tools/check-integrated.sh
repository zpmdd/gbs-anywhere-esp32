#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
idf=${IDF_PATH:?Source ESP-IDF export.sh first}
mkdir -p "$root/build-integrated-host"
cd "$root"
cc -std=c11 -Wno-deprecated-declarations -fsanitize=address,undefined -g -I "$idf/components/json/cJSON" -c "$idf/components/json/cJSON/cJSON.c" -o build-integrated-host/cJSON.o
for name in logic worker display_model hub hub_data; do
 case "$name" in
 logic|worker) sources="integrated/main/logic.c" ;;
 display_model) sources="integrated/display/gbs_ui_model.c" ;;
 *) sources="integrated/display/gbs_hub.c integrated/display/gbs_ui_model.c" ;;
 esac
 cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
   -I integrated/tests/stubs -I integrated/main -I integrated/display -I "$idf/components/json/cJSON" \
   $sources "integrated/tests/test_$name.c" build-integrated-host/cJSON.o -lm -o "build-integrated-host/test_$name"
 "build-integrated-host/test_$name" integrated/tests/fixtures
done
python tools/test_integrated_surface.py
python tools/test_schedule_control.py
python tools/test_command_coordinator.py
python tools/test_quick_weight_control.py
python tools/test_integrated_support.py
python tools/test_state_selection.py
python tools/test_display_preview.py
python tools/test_recipe_log.py
python tools/test_recipe_follow.py
python tools/test_record_grinder_recipe.py
python tools/test_brew_timer_state_machine.py
python tools/test_command_deadline.py
python tools/test_statistics_cache.py
python tools/test_stomp_codec.py
python tools/test_websocket_reassembly.py
python tools/test_http_response_accumulator.py
python tools/test_cloud_websocket_policy.py
