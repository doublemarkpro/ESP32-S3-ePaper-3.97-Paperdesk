"""Local authorization boundaries; never use an account or contact Garmin."""
import http.client
from html.parser import HTMLParser
import importlib.util
import json
import logging
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[1]
probe_module = None
SKIP_REASON = 'Garmin probe tests require Python 3.12+ and optional garminconnect dependencies'
if sys.version_info >= (3, 12):
    previous_logging_level = logging.root.manager.disable
    try:
        spec = importlib.util.spec_from_file_location('paperdesk_garmin_probe', ROOT / 'tools/garmin_probe.py')
        probe_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe_module)
    except ModuleNotFoundError:
        probe_module = None
    finally:
        # Importing the login helper suppresses credential-bearing library logs;
        # importing its tests must not change logging for the other test suites.
        logging.disable(previous_logging_level)


@unittest.skipUnless(probe_module is not None, SKIP_REASON)
class GarminProbeBoundaryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='paperdesk-garmin-test-')
        self.addCleanup(temporary.cleanup)
        private = Path(temporary.name)
        for attribute, value in [('PRIVATE', private), ('TOKEN_FILE', private / 'garmin_tokens.json')]:
            patcher = patch.object(probe_module, attribute, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        # Defense in depth: even a regression bypassing the mocked start()
        # cannot create a real Garmin client or make a requests network call.
        for patcher in (
            patch.object(probe_module, 'Garmin', side_effect=AssertionError('Real Garmin login forbidden in tests')),
            patch.object(probe_module.requests.Session, 'request',
                         side_effect=AssertionError('External requests forbidden in tests')),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)
        self.probe = probe_module.Probe()
        self.probe.start = Mock(return_value=True)
        self.server = probe_module.ThreadingHTTPServer(('127.0.0.1', 0), probe_module.Handler)
        self.server.expected_host = '127.0.0.1:' + str(self.server.server_port)
        self.server.probe = self.probe
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={'poll_interval': 0.01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def login(self, changes=None, omitted=()):
        headers = {
            'Host': self.server.expected_host,
            'Origin': 'http://' + self.server.expected_host,
            'Content-Type': 'application/json',
            'X-Probe-CSRF': self.probe.csrf,
        }
        headers.update(changes or {})
        for key in omitted:
            headers.pop(key)
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=3)
        try:
            connection.request('POST', '/api/login',
                               body=json.dumps({'email': ' 13800000000 ', 'password': 'synthetic-test-password'}),
                               headers=headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def test_wrong_host_cannot_start_login(self):
        for host in ('evil.example', self.server.expected_host + '.evil.example', 'localhost:' + str(self.server.server_port)):
            with self.subTest(host=host):
                status, _ = self.login({'Host': host})
                self.assertEqual(status, 403)
                self.probe.start.assert_not_called()

    def test_foreign_or_missing_origin_cannot_start_login(self):
        for origin in ('https://evil.example', 'http://' + self.server.expected_host + '.evil.example', 'null'):
            with self.subTest(origin=origin):
                status, _ = self.login({'Origin': origin})
                self.assertEqual(status, 403)
                self.probe.start.assert_not_called()
        self.assertEqual(self.login(omitted=('Origin',))[0], 403)
        self.probe.start.assert_not_called()

    def test_wrong_or_missing_csrf_cannot_start_login(self):
        self.assertEqual(self.login({'X-Probe-CSRF': 'wrong-token'})[0], 403)
        self.assertEqual(self.login(omitted=('X-Probe-CSRF',))[0], 403)
        self.probe.start.assert_not_called()

    def test_phone_username_is_accepted_without_network_or_credential_echo(self):
        status, body = self.login()
        self.assertEqual(status, 202)
        self.probe.start.assert_called_once_with('13800000000', 'synthetic-test-password')
        self.assertEqual(body, {'ok': True})
        self.assertEqual(list(probe_module.PRIVATE.iterdir()), [])


@unittest.skipUnless(probe_module is not None, SKIP_REASON)
class GarminProbePrivacyTests(unittest.TestCase):
    def test_errors_keep_only_type_and_http_status_not_secret_text(self):
        secret = 'private-password-and-ticket-ST-test-only'
        for error in (ValueError(secret), RuntimeError('HTTP 401 ' + secret),
                      probe_module.ProbeError('failed: 403 ' + secret)):
            with self.subTest(kind=type(error).__name__):
                rendered = probe_module.safe_error(error)
                self.assertNotIn(secret, rendered)
                self.assertNotIn('ST-test-only', rendered)
                self.assertRegex(rendered, r'^[A-Za-z]+(?: / HTTP [45]\d\d)?$')
        self.assertEqual(probe_module.safe_error(RuntimeError('HTTP 401 ' + secret)),
                         'RuntimeError / HTTP 401')
        self.assertEqual(probe_module.safe_error(probe_module.ProbeError('MFA timeout')),
                         '验证码等待超时，请重新登录')

    def test_login_form_does_not_require_an_email_address(self):
        class Inputs(HTMLParser):
            def __init__(self):
                super().__init__()
                self.by_id = {}

            def handle_starttag(self, tag, attrs):
                attributes = dict(attrs)
                if tag == 'input':
                    self.by_id[attributes.get('id')] = attributes

        parser = Inputs()
        parser.feed((ROOT / 'tools/garmin_probe.html').read_text(encoding='utf-8'))
        account = parser.by_id['email']
        self.assertIn(account.get('type', 'text'), ('text', 'tel'))
        self.assertNotIn('@', account.get('pattern', ''))
        self.assertEqual(parser.by_id['password'].get('type'), 'password')


if __name__ == '__main__':
    unittest.main()
