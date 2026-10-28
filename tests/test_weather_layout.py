"""Exercise the firmware's pure C++ weather layout helpers on the host."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class WeatherLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('g++ is not on PATH; C++ weather layout tests require it')
        temporary = tempfile.TemporaryDirectory(prefix='paperdesk-weather-test-')
        cls.addClassCleanup(temporary.cleanup)
        cls.executable = Path(temporary.name) / ('weather_layout_test.exe' if os.name == 'nt' else 'weather_layout_test')
        result = subprocess.run(
            [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-pedantic',
             '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
             '-I', str(ROOT / 'firmware/main'),
             str(ROOT / 'tests/weather_layout_test.cpp'), '-o', str(cls.executable)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
        if result.returncode:
            raise AssertionError(f'Weather layout test compilation failed:\n{result.stdout}\n{result.stderr}')

    def run_case(self, case):
        result = subprocess.run([str(self.executable), case], capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=10)
        self.assertEqual(result.returncode, 0, f'{case}:\n{result.stdout}\n{result.stderr}')

    def test_temperature_accepts_real_zero_and_signed_boundaries(self):
        self.run_case('temperature')

    def test_missing_invalid_and_reversed_ranges_remain_unavailable(self):
        self.run_case('invalid_ranges')

    def test_shared_axis_handles_positive_negative_and_mixed_days(self):
        self.run_case('signed_axes')

    def test_same_temperature_has_a_nonzero_axis_span(self):
        self.run_case('same_temperature')

    def test_all_days_share_proportional_mapping_and_endpoints(self):
        self.run_case('projection')

    def test_calendar_validation_includes_leap_centuries(self):
        self.run_case('calendar')

    def test_cached_dates_are_not_relabelled_by_row_position(self):
        self.run_case('cached_dates')

    def test_relative_day_labels_cross_leap_day_and_year_boundary(self):
        self.run_case('date_boundaries')


if __name__ == '__main__':
    unittest.main()
