"""Only the physically verified native ESP32-S3 USB device may be opened.
CH340 (VID 1A86) is never selected or opened, regardless of COM number.
"""
import argparse, hashlib, json, os, subprocess, sys, time
from pathlib import Path
from serial.tools import list_ports
ROOT=Path(__file__).resolve().parents[1]

def native_ports():
    return [p for p in list_ports.comports() if p.vid==0x303a and p.pid==0x1001]


def expected_serial():
    serial_number=os.environ.get('PAPERDESK_SERIAL','').strip()
    local=ROOT/'.local/device.json'
    if not serial_number and local.exists():
        value=json.loads(local.read_text(encoding='utf-8')).get('serial','')
        if not isinstance(value,str):raise RuntimeError('本地设备配置中的serial必须为字符串')
        serial_number=value.strip()
    if not serial_number:
        candidates=', '.join(f'{p.device} serial={p.serial_number}' for p in native_ports()) or '无'
        raise RuntimeError('请先通过PAPERDESK_SERIAL或.local/device.json指定已确认的设备序列号；不会打开串口。原生ESP候选：'+candidates)
    return serial_number.upper()

def target_port():
    expected=expected_serial()
    ports=[p for p in native_ports() if (p.serial_number or '').upper()==expected]
    if len(ports)!=1:raise RuntimeError('未找到唯一的已确认ESP32设备；不会操作其他串口')
    return ports[0].device

def run_esptool(*args):
    return subprocess.run([sys.executable,'-m','esptool','--chip','esp32s3','--port',target_port(),*args],check=True)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('action',choices=['identify','backup','flash','monitor']);parser.add_argument('--skip-backup',action='store_true',help='Flash without a backup when explicitly requested');args=parser.parse_args()
    expected=expected_serial();port=target_port();print('Verified native ESP32-S3:',port,'serial',expected,flush=True)
    if args.action=='identify':return run_esptool('flash-id')
    if args.action=='backup':
        backup=ROOT/'backups'/('original-'+time.strftime('%Y%m%d-%H%M%S')+'.bin');backup.parent.mkdir(exist_ok=True)
        run_esptool('--baud','460800','read-flash','0','0x1000000',str(backup))
        if backup.stat().st_size!=16*1024*1024:raise RuntimeError('备份长度错误，不可烧录')
        manifest={'file':backup.name,'serial':expected,'size':backup.stat().st_size,'sha256':hashlib.sha256(backup.read_bytes()).hexdigest()}
        (ROOT/'backups/manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8');return
    if args.action=='flash':
        if not args.skip_backup:
            manifest=json.loads((ROOT/'backups/manifest.json').read_text(encoding='utf-8'));backup=ROOT/'backups'/manifest['file']
            if manifest['serial']!=expected or backup.stat().st_size!=16*1024*1024 or hashlib.sha256(backup.read_bytes()).hexdigest()!=manifest['sha256']:raise RuntimeError('备份校验失败，取消烧录')
        build=ROOT/'firmware/build';flasher=json.loads((build/'flasher_args.json').read_text())
        command=['--baud','460800','write-flash','--flash-mode','dio','--flash-size','16MB']
        for offset,file in flasher['flash_files'].items():command.extend([offset,str(build/file)])
        return run_esptool(*command)
    if args.action=='monitor':
        import serial
        with serial.Serial(port,115200,timeout=1) as s:
            s.dtr=False;s.rts=False
            try:
                while True:
                    line=s.readline()
                    if line:print(line.decode('utf-8',errors='replace').rstrip(),flush=True)
            except KeyboardInterrupt:pass

if __name__=='__main__':main()
