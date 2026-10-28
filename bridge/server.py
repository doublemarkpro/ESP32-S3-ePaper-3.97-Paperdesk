"""Paperdesk LAN bridge. Python 3.10+, standard library only.
Uses Codex app-server account RPC; never reads/copies auth.json or sends inference.
"""
from __future__ import annotations
import argparse, copy, gzip, hmac, io, json, math, os, queue, secrets
import shutil, subprocess, threading, time, urllib.error, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
LOCAL=ROOT/'.local'
CONFIG=LOCAL/'gateway.json'
LOCK=threading.RLock()
STATE={'schema':1,'generated_at':0,'weather':{'ok':False,'error':'天气未配置'},'codex':{'ok':False,'error':'正在读取Codex账号'}}
STOP=threading.Event()
WAKE=threading.Event()

def atomic_write(path:Path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    tmp=path.with_suffix(path.suffix+'.tmp')
    tmp.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8')
    os.replace(tmp,path)

def load_config():
    if CONFIG.exists():return json.loads(CONFIG.read_text(encoding='utf-8'))
    c={'token':secrets.token_urlsafe(24),'weather':{'host':'','key':'','location':'101020100','city':'上海'}}
    atomic_write(CONFIG,c)
    return c

def normalize_limits(result):
    """Null is unavailable, never 0%; preserve all actual buckets and windows."""
    buckets=result.get('rateLimitsByLimitId')
    if not isinstance(buckets,dict) or not buckets:
        legacy=result.get('rateLimits')
        buckets={legacy.get('limitId','codex'):legacy} if isinstance(legacy,dict) else {}
    windows=[]
    for bucket_id,bucket in buckets.items():
        if not isinstance(bucket,dict):continue
        for name in ('primary','secondary'):
            w=bucket.get(name)
            if not isinstance(w,dict):continue
            used=w.get('usedPercent');minutes=w.get('windowDurationMins')
            if isinstance(used,bool) or not isinstance(used,(int,float)) or not math.isfinite(used):continue
            if isinstance(minutes,bool) or not isinstance(minutes,(int,float)) or not math.isfinite(minutes) or minutes<=0:minutes=None
            used=max(0,min(100,used))
            label=(f'{minutes//1440:g}天窗口' if isinstance(minutes,(int,float)) and minutes%1440==0 else f'{minutes/60:g}小时窗口' if isinstance(minutes,(int,float)) and minutes%60==0 else f'{minutes:g}分钟窗口' if isinstance(minutes,(int,float)) else '额度窗口')
            resets=w.get('resetsAt')
            if isinstance(resets,bool) or not isinstance(resets,(int,float)) or not math.isfinite(resets) or resets<=0:resets=0
            windows.append({'bucket':bucket_id,'label':label,'used':round(used),'remaining':round(100-used),'resets_at':resets})
    return {'ok':bool(windows),'error':'' if windows else '账号未返回可用额度窗口','windows':windows,'updated_at':int(time.time())}

def codex_limits(timeout=15):
    executable=shutil.which('codex.exe') or shutil.which('codex')
    if not executable:raise RuntimeError('电脑未找到Codex命令，请先安装并登录')
    flags=getattr(subprocess,'CREATE_NO_WINDOW',0)
    p=subprocess.Popen([executable,'app-server'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,encoding='utf-8',creationflags=flags)
    output=queue.Queue()
    def read():
        for line in p.stdout:
            try:output.put(json.loads(line))
            except json.JSONDecodeError:pass
    threading.Thread(target=read,daemon=True).start()
    def rpc(method,request_id,params=None):
        message={'id':request_id,'method':method}
        if params is not None:message['params']=params
        p.stdin.write(json.dumps(message)+'\n');p.stdin.flush()
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            try:answer=output.get(timeout=min(1,max(.01,deadline-time.monotonic())))
            except queue.Empty:
                if p.poll() is not None:raise RuntimeError('Codex本机服务已退出')
                continue
            if answer.get('id')==request_id:
                if 'error' in answer:raise RuntimeError('Codex额度读取失败，请检查账号登录状态')
                return answer.get('result',{})
        raise TimeoutError('Codex本机额度读取超时')
    try:
        rpc('initialize',1,{'clientInfo':{'name':'paperdesk','version':'0.2.0'},'capabilities':{'experimentalApi':True}})
        p.stdin.write('{"method":"initialized"}\n');p.stdin.flush()
        return normalize_limits(rpc('account/rateLimits/read',2))
    finally:
        if p.poll() is None:p.terminate()
        try:p.wait(timeout=3)
        except subprocess.TimeoutExpired:p.kill();p.wait()
        if p.stdin:p.stdin.close()
        if p.stdout:p.stdout.close()

def http_json(url,*,headers=None,data=None):
    hdr={'User-Agent':'Paperdesk/0.2.0','Accept-Encoding':'gzip','Accept':'application/json',**(headers or {})}
    req=urllib.request.Request(url,data=data,headers=hdr)
    with urllib.request.urlopen(req,timeout=12) as response:
        raw=response.read(1024*1024+1)
        if len(raw)>1024*1024:raise RuntimeError('接口响应超过大小限制')
        if response.headers.get('Content-Encoding')=='gzip':
            with gzip.GzipFile(fileobj=io.BytesIO(raw)) as compressed:raw=compressed.read(2*1024*1024+1)
    if len(raw)>2*1024*1024:raise RuntimeError('解压后响应过大')
    return json.loads(raw)

def weather_fetch(config):
    w=config['weather'];host=w.get('host','').strip().lower()
    if not host or not w.get('key'):return {'ok':False,'error':'请在网关设置中填写和风天气Host和Key'}
    if not host.endswith('.qweatherapi.com') or any(c in host for c in '/:@?# '):raise ValueError('和风天气Host应为专属qweatherapi.com域名')
    location=urllib.parse.quote(w.get('location','101020100'),safe=',.')
    headers={'X-QW-Api-Key':w['key']}
    now=http_json(f'https://{host}/v7/weather/now?location={location}&lang=zh',headers=headers)
    daily=http_json(f'https://{host}/v7/weather/3d?location={location}&lang=zh',headers=headers)
    if now.get('code')!='200' or daily.get('code')!='200':raise RuntimeError(f'和风天气返回状态 {now.get("code")} / {daily.get("code")}，请检查账号权限')
    n=now.get('now',{})
    return {'ok':True,'city':w.get('city',''),'temp':str(n.get('temp','--')),'text':n.get('text',''),'icon':str(n.get('icon') or ''),'feels_like':str(n.get('feelsLike','--')),'wind':n.get('windDir',''),'wind_scale':str(n.get('windScale','--')),'humidity':str(n.get('humidity','--')),'update':now.get('updateTime',''),'daily':[{'text':d.get('textDay',''),'icon':str(d.get('iconDay') or ''),'date':str(d.get('fxDate') or ''),'min':str(d.get('tempMin','--')),'max':str(d.get('tempMax','--'))} for d in daily.get('daily',[])[:3]],'updated_at':int(time.time())}

def refresh_loop():
    weather_cache=None;weather_fingerprint=None;weather_at=0
    while not STOP.is_set():
        with LOCK:config=copy.deepcopy(load_config());old=copy.deepcopy(STATE)
        result={'schema':1,'generated_at':int(time.time())}
        try:result['codex']=codex_limits()
        except Exception as e:result['codex']={'ok':False,'error':str(e)[:120]}
        now=time.time();fingerprint=json.dumps(config['weather'],sort_keys=True)
        try:
            if weather_cache is None or fingerprint!=weather_fingerprint or now-weather_at>=1800:
                weather_cache=weather_fetch(config);weather_at=now;weather_fingerprint=fingerprint
            result['weather']=copy.deepcopy(weather_cache)
        except Exception as e:
            result['weather']=copy.deepcopy(weather_cache) if weather_cache and fingerprint==weather_fingerprint else {'ok':False}
            result['weather']['error']=str(e)[:120];result['weather']['stale']=True
        # Keep the last valid quota on failures; freshness is explicit in its fields.
        if not result['codex']['ok'] and old.get('codex',{}).get('ok'):
            failure=result['codex']['error'];result['codex']=old['codex'];result['codex']['stale']=True;result['codex']['error']=failure
        with LOCK:STATE.clear();STATE.update(result)
        WAKE.wait(300);WAKE.clear()

def public_config(c):
    # Explicitly expose only active settings. Older local configuration keys are
    # retained on disk by updates, but never returned through the current API.
    weather=c.get('weather',{})
    return {'weather':{**{key:weather.get(key,'') for key in ('host','location','city')},'key_set':bool(weather.get('key'))}}


def update_config(config,body):
    if not isinstance(body,dict):raise ValueError('需要JSON对象')
    if set(body)-{'weather'}:raise ValueError('不支持的配置分组')
    values=body.get('weather',{})
    if not isinstance(values,dict):raise ValueError('无效配置分组')
    fields={'host':120,'key':256,'location':64,'city':24}
    if set(values)-set(fields):raise ValueError('不支持的配置字段')
    updated=copy.deepcopy(config)
    for field,limit in fields.items():
        if field not in values:continue
        value=values[field]
        if not isinstance(value,str) or len(value)>limit or any(ord(c)<32 for c in value):raise ValueError('字段类型或长度不正确')
        value=value.strip()
        if field!='key' or value:updated.setdefault('weather',{})[field]=value
    return updated


class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass # Never log headers or credentials.
    def authorized(self):
        expected=load_config()['token'];actual=self.headers.get('Authorization','').removeprefix('Bearer ')
        return hmac.compare_digest(actual.encode('utf-8'),expected.encode('utf-8'))
    def reply(self,status,data,kind='application/json; charset=utf-8'):
        raw=data.encode() if isinstance(data,str) else json.dumps(data,ensure_ascii=False).encode()
        self.send_response(status);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(raw)));self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'");self.end_headers();self.wfile.write(raw)
    def do_GET(self):
        if self.path=='/':return self.reply(200,(ROOT/'bridge/web.html').read_text(encoding='utf-8'),'text/html; charset=utf-8')
        if not self.authorized():return self.reply(401,{'error':'设备令牌不正确'})
        with LOCK:
            if self.path=='/v1/snapshot':return self.reply(200,copy.deepcopy(STATE))
            if self.path=='/v1/config':return self.reply(200,public_config(load_config()))
        self.reply(404,{'error':'not found'})
    def do_POST(self):
        if not self.authorized():return self.reply(401,{'error':'设备令牌不正确'})
        if self.path=='/v1/refresh':WAKE.set();return self.reply(202,{'ok':True})
        if self.path!='/v1/config':return self.reply(404,{'error':'not found'})
        try:
            length=int(self.headers.get('Content-Length','0'))
            if not 0<length<=16384:raise ValueError('请求过大或为空')
            body=json.loads(self.rfile.read(length))
            with LOCK:
                config=update_config(load_config(),body)
                atomic_write(CONFIG,config)
            WAKE.set();return self.reply(200,{'ok':True})
        except (ValueError,TypeError,json.JSONDecodeError) as e:return self.reply(400,{'error':str(e)})

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--host',default='127.0.0.1');parser.add_argument('--port',type=int,default=8787);parser.add_argument('--once',action='store_true');args=parser.parse_args();load_config()
    if args.once:print(json.dumps(codex_limits(),ensure_ascii=False));return
    thread=threading.Thread(target=refresh_loop,daemon=True);thread.start();server=ThreadingHTTPServer((args.host,args.port),Handler)
    print(f'Paperdesk gateway: http://{args.host}:{args.port}; token is stored in {CONFIG} (not printed).',flush=True)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:STOP.set();WAKE.set();server.server_close()

if __name__=='__main__':main()
