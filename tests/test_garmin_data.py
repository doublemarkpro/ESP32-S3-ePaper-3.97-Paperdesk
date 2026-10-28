"""Test the production Garmin parsers with synthetic JSON; no accounts/network."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GarminDataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('g++ or clang++ is required for Garmin data tests')
        temporary = tempfile.TemporaryDirectory(prefix='paperdesk-garmin-data-test-')
        cls.addClassCleanup(temporary.cleanup)
        cls.executable = Path(temporary.name) / ('garmin_data_test.exe' if os.name == 'nt' else 'garmin_data_test')
        result = subprocess.run(
            [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-pedantic',
             '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
             '-I', str(ROOT / 'firmware/main'), '-I', str(ROOT / 'firmware/components/json'),
             str(ROOT / 'tests/garmin_data_test.cpp'), str(ROOT / 'firmware/components/json/cJSON.c'),
             '-o', str(cls.executable)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
        if result.returncode:
            raise AssertionError(f'Garmin data test compilation failed:\n{result.stdout}\n{result.stderr}')

    def run_case(self, case):
        result = subprocess.run([str(self.executable), case], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=10)
        self.assertEqual(result.returncode, 0, f'{case}:\n{result.stdout}\n{result.stderr}')

    def test_cloud_time_seconds_milliseconds_and_utc_do_not_apply_timezone_twice(self):
        self.run_case('cloud_time')

    def test_invalid_or_unknown_cloud_time_stays_unknown(self):
        self.run_case('invalid_time')

    def test_new_date_does_not_inherit_yesterday_sleep_hrv_or_other_metrics(self):
        self.run_case('rollover')

    def test_same_day_retained_metrics_are_marked_cached(self):
        self.run_case('same_day')

    def test_invalid_summary_keeps_previous_result_and_missing_is_not_zero(self):
        self.run_case('invalid_summary')

    def test_sleep_requires_matching_calendar_date_and_preserves_valid_same_day_data(self):
        self.run_case('sleep_dates')

    def test_hrv_requires_matching_calendar_date_and_valid_measurement(self):
        self.run_case('hrv_dates')

    def test_body_battery_uses_latest_valid_timestamp_for_requested_date(self):
        self.run_case('battery')

    def test_latest_activity_keeps_its_own_date_and_valid_empty_list_clears_it(self):
        self.run_case('activity')


if __name__ == '__main__':
    unittest.main()
