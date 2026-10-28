#!/usr/bin/env python3
"""Prepare private first-install NVS from a retired companion; never flashes a board."""
import argparse
import base64
import csv
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import uuid


def text(config, key, limit, empty=False):
    value=config.get(key)
    if not isinstance(value,str) or (not empty and not value) or len(value.encode())>limit or '\0' in value:
        raise ValueError('invalid '+key)
    return value


def rows(config, token, cloud=False, grinder=False):
    ssid=text(config,'wifi_ssid',32)
    password=text(config,'wifi_password',64,empty=True)
    result=[['key','type','encoding','value'],['lamabuddy','namespace','',''],
            ['wifi_ssid','data','string',ssid],['wifi_pass','data','string',password],
            ['local_token','data','string',token]]
    if cloud or grinder:
        serial=text(config,'serial',64)
        if not re.fullmatch(r'[A-Za-z0-9-]+',serial): raise ValueError('invalid serial')
    if cloud:
        installation={key:text(config,key,256) for key in ['installation_id','private_key','secret']}
        uuid.UUID(installation['installation_id'])
        if len(base64.b64decode(installation['secret'],validate=True))!=32: raise ValueError('invalid secret')
        if not base64.b64decode(installation['private_key'],validate=True): raise ValueError('invalid private_key')
        packed=json.dumps(installation,separators=(',',':'))
        if len(packed)>768: raise ValueError('installation key exceeds Hub capacity')
        for key,value in [('cloud_username',text(config,'cloud_username',96)),('cloud_password',text(config,'cloud_password',128)),
                          ('cloud_inst_key',packed),('cloud_serial',serial),('machine_serial',serial),('machine_name','Coffee machine')]:
            result.append([key,'data','string',value])
    if grinder:
        import ipaddress
        minimal={'schema':2,'serial':serial}
        for key,limit in [('grinder_host',15),('grinder_cert',4095),('brew_device_id',36)]: minimal[key]=text(config,key,limit)
        if ipaddress.ip_address(minimal['grinder_host']).version!=4: raise ValueError('grinder IPv4 required')
        uuid.UUID(minimal['brew_device_id'])
        cert=minimal['grinder_cert']
        if not cert.startswith('-----BEGIN CERTIFICATE-----') or '-----END CERTIFICATE-----' not in cert: raise ValueError('pinned grinder certificate required')
        pairing=config.get('pairing_id')
        if type(pairing) is not int or not 1<=pairing<=65535: raise ValueError('invalid pairing_id')
        minimal['pairing_id']=pairing
        result.extend([['gbs_cfg','namespace','',''],['config','data','string',json.dumps(minimal,separators=(',',':'))],['active','data','u8',0]])
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path,help='New private directory; must not exist')
    parser.add_argument('--profile',choices=['touchscreen','amoled216','mosaico','headless'],default='touchscreen',
                        help='Partition layout for the new board (default: touchscreen)')
    parser.add_argument('--include-cloud',action='store_true',help='Use only the retired device cloud identity')
    parser.add_argument('--include-grinder',action='store_true',help='Import retired pairing in monitor mode')
    args=parser.parse_args()
    os.umask(0o077)
    try:
        config=json.loads(args.source.read_text())
        token=secrets.token_hex(16)
        data=rows(config,token,args.include_cloud,args.include_grinder)
        idf=Path(os.environ.get('IDF_PATH',Path.home()/'esp/esp-idf'))
        generator=idf/'components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py'
        if not generator.is_file(): raise ValueError('ESP-IDF NVS generator unavailable')
        args.output.mkdir(mode=0o700,parents=True,exist_ok=False)
        with (args.output/'nvs.csv').open('w',newline='') as stream: csv.writer(stream).writerows(data)
        (args.output/'hub-token.txt').write_text(token+'\n')
        size='0x6000' if args.profile=='headless' else '0x15000'
        subprocess.run([sys.executable,str(generator),'generate',str(args.output/'nvs.csv'),str(args.output/'nvs.bin'),size],check=True,stdout=subprocess.DEVNULL)
        print('Private NVS prepared; cloud='+str(args.include_cloud)+' grinder='+str(args.include_grinder)+' active=False. No device was changed.')
    except (ValueError,OSError) as error:
        # Values and vendor replies must never be echoed by the provisioning CLI.
        parser.exit(2,'Preparation failed: '+type(error).__name__+'; check source fields and output path.\n')
if __name__=='__main__': main()
