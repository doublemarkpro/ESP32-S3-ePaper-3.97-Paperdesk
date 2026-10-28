"""Run the authenticated ESP32 probe without forwarding Garmin traffic.

Uses private .local/device-access.json and the local authorization tool's tokens.
Never prints credentials or raw health responses. Only talks to a private-IP device.
"""
import argparse
import ipaddress
import json
import time
import urllib.request
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / '.local' / 'garmin-probe'


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('stage', choices=['initial', 'resume', 'status'])
    args = parser.parse_args()
    access = json.loads((ROOT / '.local' / 'device-access.json').read_text(encoding='utf-8'))
    url = access['url'].rstrip('/')
    parsed = urlsplit(url)
    if (parsed.scheme not in ('http', 'https') or parsed.username or parsed.password or
        parsed.query or parsed.fragment or parsed.path or not ipaddress.ip_address(parsed.hostname).is_private):
        raise RuntimeError('Device URL must be the configured private-IP device')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def request(path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url + path, data=data,
            headers={'Content-Type': 'application/json', 'X-Paperdesk-Token': access['admin']})
        with opener.open(req, timeout=20) as response:
            return json.load(response)

    def report(result):
        safe = {k: result[k] for k in ('busy', 'phase', 'core_passed', 'saved', 'error', 'checks') if k in result}
        print(json.dumps(safe, ensure_ascii=True), flush=True)

    if args.stage == 'status':
        report(request('/api/garmin-probe'))
        return
    original = request('/api/config')
    PRIVATE.mkdir(exist_ok=True)
    (PRIVATE / 'device-config-before-probe.json').write_text(json.dumps(original), encoding='utf-8')
    if args.stage == 'initial':
        body = json.loads((PRIVATE / 'garmin_tokens.json').read_text(encoding='utf-8'))
        if not all(isinstance(body.get(k), str) and body[k] for k in ('di_token', 'di_refresh_token', 'di_client_id')):
            raise RuntimeError('Missing complete DI authorization')
    else:
        body = {'action': 'resume'}
    try:
        request('/api/config', {'gateway': ''})
        assert request('/api/config')['gateway'] == ''
        request('/api/garmin-probe', body)
        print('ESP32 probe started with gateway URL empty.', flush=True)
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            result = request('/api/garmin-probe')
            if not result.get('busy'):
                break
            time.sleep(2)
        else:
            raise RuntimeError('Device probe timeout; inspect device result before retrying')
        serialized = json.dumps(result)
        if args.stage == 'initial' and any(body[k] in serialized for k in ('di_token', 'di_refresh_token')):
            raise RuntimeError('Credential unexpectedly present in probe response')
        (PRIVATE / ('device-' + args.stage + '.json')).write_text(serialized, encoding='utf-8')
        report(result)
        if result.get('can_save'):
            request('/api/garmin-probe', {'action': 'save'})
            saved = request('/api/garmin-probe')
            (PRIVATE / ('device-' + args.stage + '.json')).write_text(json.dumps(saved), encoding='utf-8')
            report(saved)
        if not result.get('core_passed'):
            raise RuntimeError('ESP32 direct probe did not pass')
        assert request('/api/config')['gateway'] == ''
        print('Direct GET + refresh + GET passed; updated device authorization saved.', flush=True)
    finally:
        request('/api/config', {'gateway': original['gateway']})
        assert request('/api/config') == original
        print('Original device settings restored.', flush=True)


if __name__ == '__main__':
    main()
