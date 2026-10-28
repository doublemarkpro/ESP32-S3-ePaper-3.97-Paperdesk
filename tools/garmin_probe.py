"""Loopback-only Garmin China login and plain-HTTPS feasibility check.

Run with Python 3.12+ and garminconnect 0.3.20. Password/MFA stay in memory.
Only tokens and a selected, private result are saved under ignored .local/.
No daemon forwarding, account writes, telemetry, or automatic login retries.
"""
import argparse
import base64
import hmac
import json
import logging
import re
import secrets
import threading
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, urlsplit
from zoneinfo import ZoneInfo

import requests
from garminconnect import Garmin
from garminconnect.client.transport import _native_headers

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / '.local' / 'garmin-probe'
TOKEN_FILE = PRIVATE / 'garmin_tokens.json'
SOURCE_COMMIT = '3eee7a4db6c2242967aae1780b8839599ea7dcbb'
API_HOST = 'https://connectapi.garmin.cn'
AUTH_URL = 'https://diauth.garmin.cn/di-oauth2-service/oauth/token'
CN = ZoneInfo('Asia/Shanghai')
logging.disable(logging.CRITICAL)  # Upstream exceptions may contain login tickets.


def write_private(name, value):
    PRIVATE.mkdir(parents=True, exist_ok=True)
    path = PRIVATE / name
    temp = path.with_suffix('.tmp')
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    temp.replace(path)


def safe_error(exc):
    # Never return str(exc): it can contain an account name, ticket or response.
    text = str(exc)
    known = {
        'MFA timeout': '验证码等待超时，请重新登录',
        'Login did not yield refreshable DI tokens': '未取得可续期的 DI 令牌',
        'Unexpected host': '请求域名校验失败',
        'Response exceeds 2 MiB': '响应超出大小上限',
        'Missing display name': '用户资料缺少必要字段',
        'No valid daily summary': '当日汇总缺少必要字段',
        'Invalid refresh response': '续期响应缺少有效令牌',
        'No valid summary after refresh': '续期后的汇总缺少必要字段',
    }
    if isinstance(exc, ProbeError) and text in known:
        return known[text]
    status = re.search(r'(?:HTTP\s*|status[=: ]+|failed: )([45]\d\d)\b', text, re.I)
    return type(exc).__name__ + (f' / HTTP {status[1]}' if status else '')


class ProbeError(Exception):
    pass


class Probe:
    def __init__(self):
        self.lock = threading.RLock()
        self.csrf = secrets.token_urlsafe(32)
        self.mfa_event = threading.Event()
        self.mfa_code = None
        self.state = {'phase': 'idle', 'message': '请在本机输入佳明国区账号。', 'checks': [], 'snapshot': {}}

    def public(self):
        with self.lock:
            return json.loads(json.dumps(self.state))

    def update(self, **values):
        with self.lock:
            self.state.update(values)
            write_private('report.json', self.state)

    def check(self, name, **result):
        with self.lock:
            self.state['checks'].append({'name': name, **result})
            write_private('report.json', self.state)

    def mfa(self):
        self.mfa_event.clear()
        self.update(phase='mfa', message='佳明要求验证码，请在下方输入本次验证码。')
        if not self.mfa_event.wait(600):
            raise ProbeError('MFA timeout')
        with self.lock:
            code, self.mfa_code = self.mfa_code, None
        self.update(phase='running', message='正在提交验证码并验证授权……')
        return code

    def start(self, email=None, password=None, resume=False):
        with self.lock:
            if self.state['phase'] in ('running', 'mfa'):
                return False
            self.state = {'phase': 'running', 'message': '正在连接佳明国区……',
                          'date': datetime.now(CN).date().isoformat(), 'checks': [],
                          'snapshot': {}, 'source_commit': SOURCE_COMMIT,
                          'transport': 'requests / standard TLS; no proxy; no API cookies'}
        threading.Thread(target=self.run, args=(email, password, resume), daemon=True).start()
        return True

    def run(self, email, password, resume):
        garmin = None
        try:
            garmin = Garmin(is_cn=True, retry_attempts=0)
            if resume:
                garmin.client.load(str(TOKEN_FILE))
            else:
                garmin.client.login(email, password, prompt_mfa=self.mfa)
            email = password = None
            client = garmin.client
            if not all(isinstance(v, str) and v for v in
                       (client.di_token, client.di_refresh_token, client.di_client_id)):
                raise ProbeError('Login did not yield refreshable DI tokens')
            client.dump(str(TOKEN_FILE))
            self.check('国区授权', ok=True, refresh_token_present=True)
            self.update(message='已授权，正在用普通 HTTPS 读取资料和当日汇总……')
            session = requests.Session()
            session.trust_env = False
            session.headers.clear()

            def request_json(label, method, url, headers, data=None, params=None):
                if urlsplit(url).netloc not in ('connectapi.garmin.cn', 'diauth.garmin.cn'):
                    raise ProbeError('Unexpected host')
                session.cookies.clear()
                with session.request(method, url, headers={**headers, 'Accept-Encoding': 'identity'},
                                     params=params, data=data, timeout=(15, 45),
                                     allow_redirects=False, stream=True) as response:
                    self.check(label, http=response.status_code, ok=False)
                    if response.status_code != 200:
                        raise ProbeError(f'HTTP {response.status_code}')
                    body = bytearray()
                    for chunk in response.iter_content(16384):
                        body.extend(chunk)
                        if len(body) > 2 * 1024 * 1024:
                            raise ProbeError('Response exceeds 2 MiB')
                    value = json.loads(body)
                    with self.lock:
                        self.state['checks'][-1]['bytes'] = len(body)
                        self.state['checks'][-1]['ok'] = True
                    return value

            def get(label, path, params=None):
                return request_json(label, 'GET', API_HOST + path,
                    _native_headers({'Authorization': 'Bearer ' + client.di_token,
                                     'Accept': 'application/json'}), params=params)

            profile = get('普通 HTTPS：用户资料', '/userprofile-service/socialProfile')
            display_name = profile.get('displayName') if isinstance(profile, dict) else None
            if not isinstance(display_name, str) or not display_name:
                raise ProbeError('Missing display name')
            encoded = quote(display_name, safe='')
            date = self.state['date']
            summary_path = '/usersummary-service/usersummary/daily/' + encoded
            first = get('普通 HTTPS：续期前读取', summary_path, {'calendarDate': date})
            if not isinstance(first, dict) or 'totalSteps' not in first:
                raise ProbeError('No valid daily summary')
            self.update(message='数据读取成功，正在强制执行一次普通 HTTPS 令牌续期……')
            basic = base64.b64encode((client.di_client_id + ':').encode()).decode()
            refreshed = request_json('普通 HTTPS：令牌续期', 'POST', AUTH_URL,
                _native_headers({'Authorization': 'Basic ' + basic, 'Accept': 'application/json',
                                 'Content-Type': 'application/x-www-form-urlencoded'}),
                data={'grant_type': 'refresh_token', 'client_id': client.di_client_id,
                      'refresh_token': client.di_refresh_token})
            access = refreshed.get('access_token')
            refresh = refreshed.get('refresh_token', client.di_refresh_token)
            if not all(isinstance(v, str) and v and len(v) <= 16384 and '\n' not in v and '\r' not in v
                       for v in (access, refresh)):
                raise ProbeError('Invalid refresh response')
            client.di_token, client.di_refresh_token = access, refresh
            client.di_client_id = client._extract_client_id_from_jwt(access) or client.di_client_id
            client.dump(str(TOKEN_FILE))  # Persist rotated refresh token before any further requests.
            summary = get('普通 HTTPS：续期后重新读取', summary_path, {'calendarDate': date})
            if not isinstance(summary, dict) or 'totalSteps' not in summary:
                raise ProbeError('No valid summary after refresh')
            snapshot = {'date': date, 'fetched_at': datetime.now(CN).isoformat(),
                        'steps': summary.get('totalSteps'), 'step_goal': summary.get('dailyStepGoal'),
                        'resting_heart_rate': summary.get('restingHeartRate'),
                        'average_stress': summary.get('averageStressLevel'),
                        'body_battery_latest': summary.get('bodyBatteryMostRecentValue'),
                        'last_sync_timestamp_gmt': summary.get('lastSyncTimestampGMT')}
            self.update(core_passed=True, message='授权、读取和续期均通过；正在核对可用于简报的数据……')
            optional = [
                ('睡眠', '/wellness-service/wellness/dailySleepData/' + encoded,
                 {'date': date, 'nonSleepBufferMinutes': 60}),
                ('身体电量', '/wellness-service/wellness/bodyBattery/reports/daily',
                 {'startDate': date, 'endDate': date}),
                ('最近运动', '/activitylist-service/activities/search/activities', {'start': 0, 'limit': 1}),
                ('HRV', '/hrv-service/hrv/' + date, None),
            ]
            for label, path, params in optional:
                try:
                    value = get(label, path, params)
                    if label == '睡眠':
                        dto = value.get('dailySleepDTO') or {}
                        snapshot['sleep_seconds'] = dto.get('sleepTimeSeconds')
                        snapshot['sleep_score'] = ((dto.get('sleepScores') or {}).get('overall') or {}).get('value')
                    elif label == '身体电量':
                        rows = value if isinstance(value, list) else []
                        snapshot['body_battery_available'] = bool(rows)
                        for row in rows:
                            points = row.get('bodyBatteryValuesArray') or []
                            valid = [p for p in points if isinstance(p, list) and len(p) > 1 and
                                     isinstance(p[1], (int, float)) and 0 <= p[1] <= 100]
                            if valid:
                                snapshot['body_battery_latest'] = valid[-1][1]
                                snapshot['body_battery_sample_time'] = valid[-1][0]
                    elif label == '最近运动':
                        rows = value if isinstance(value, list) else []
                        if rows:
                            activity = rows[0]
                            snapshot['activity'] = {'type': (activity.get('activityType') or {}).get('typeKey'),
                                'start_local': activity.get('startTimeLocal'),
                                'distance_meters': activity.get('distance'), 'duration_seconds': activity.get('duration')}
                    elif label == 'HRV':
                        hrv = value.get('hrvSummary') or {}
                        snapshot['hrv_last_night'] = hrv.get('lastNightAvg')
                except Exception as exc:
                    self.check(label + '：可选数据未取得', ok=False, error=safe_error(exc))
                    if '429' in str(exc):
                        break
            write_private('snapshot.json', snapshot)
            self.update(phase='passed', snapshot=snapshot,
                        message='电脑端验证通过：国区授权、普通 HTTPS 读取及强制续期。下一步验证 ESP32 原生 TLS。')
        except Exception as exc:
            with self.lock:
                if self.state['checks']:
                    self.state['checks'][-1]['ok'] = False
            self.update(phase='failed', error=safe_error(exc),
                        message='本次验证未完成。已停止，不会自动反复登录；请保留此页供检查。')
        finally:
            email = password = None
            if garmin is not None:
                garmin.username = garmin.password = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, status, body, content_type='application/json; charset=utf-8'):
        encoded = body.encode() if isinstance(body, str) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(encoded)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Content-Security-Policy', "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(encoded)

    def host_ok(self):
        return self.headers.get('Host') == self.server.expected_host

    def do_GET(self):
        if not self.host_ok():
            return self.send(403, {'error': 'host'})
        if self.path == '/':
            html = Path(__file__).with_suffix('.html').read_text(encoding='utf-8')
            return self.send(200, html.replace('__CSRF__', self.server.probe.csrf), 'text/html; charset=utf-8')
        if self.path == '/api/status':
            return self.send(200, self.server.probe.public())
        self.send(404, {'error': 'not found'})

    def do_POST(self):
        if (not self.host_ok() or self.headers.get('Origin') != 'http://' + self.server.expected_host or
            not hmac.compare_digest(self.headers.get('X-Probe-CSRF', ''), self.server.probe.csrf)):
            return self.send(403, {'error': 'origin'})
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 8192 or not self.headers.get('Content-Type', '').startswith('application/json'):
                return self.send(400, {'error': 'body'})
            data = json.loads(self.rfile.read(size))
            if not isinstance(data, dict):
                raise ValueError()
        except (ValueError, TypeError):
            return self.send(400, {'error': 'body'})
        probe = self.server.probe
        if self.path == '/api/login':
            email, password = data.pop('email', ''), data.pop('password', '')
            if not isinstance(email, str) or not isinstance(password, str) or not email.strip() or not password:
                return self.send(400, {'error': '请输入账号和密码'})
            started = probe.start(email.strip(), password)
            email = password = None
            return self.send(202 if started else 409, {'ok': started})
        if self.path == '/api/mfa':
            code = data.pop('code', '')
            with probe.lock:
                if probe.state['phase'] != 'mfa' or not isinstance(code, str) or not re.fullmatch(r'[A-Za-z0-9 -]{4,32}', code):
                    return self.send(400, {'error': '验证码格式或状态不正确'})
                probe.mfa_code = code.strip()
                probe.mfa_event.set()
            return self.send(202, {'ok': True})
        if self.path == '/api/resume' and TOKEN_FILE.exists():
            started = probe.start(resume=True)
            return self.send(202 if started else 409, {'ok': started})
        self.send(404, {'error': 'not found'})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8791)
    args = parser.parse_args()
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.expected_host = f'127.0.0.1:{args.port}'
    server.probe = Probe()
    print(f'Garmin China probe ready at http://{server.expected_host}/', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
