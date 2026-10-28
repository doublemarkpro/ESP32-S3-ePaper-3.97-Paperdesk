"""Host tests of the actual offline Gregorian/lunar/solar-term engine."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CalendarTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('g++ is not on PATH; C++ calendar tests require it')
        temporary = tempfile.TemporaryDirectory(prefix='paperdesk-calendar-test-')
        cls.addClassCleanup(temporary.cleanup)
        cls.executable = Path(temporary.name) / ('calendar_test.exe' if os.name == 'nt' else 'calendar_test')
        result = subprocess.run(
            [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-pedantic',
             '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
             '-I', str(ROOT / 'firmware/main'),
             str(ROOT / 'tests/calendar_test.cpp'),
             str(ROOT / 'firmware/main/calendar.cpp'), '-o', str(cls.executable)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
        if result.returncode:
            raise AssertionError(f'Calendar test compilation failed:\n{result.stdout}\n{result.stderr}')

    def run_case(self, case):
        result = subprocess.run([str(self.executable), case], capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=10)
        self.assertEqual(result.returncode, 0, f'{case}:\n{result.stdout}\n{result.stderr}')

    def test_gregorian_validity_and_leap_centuries(self):
        self.run_case('gregorian')

    def test_known_months_require_four_five_or_six_rows(self):
        self.run_case('month_layout')

    def test_monday_first_weekday_alignment(self):
        self.run_case('weekdays')

    def test_month_navigation_crosses_years_and_rejects_overflow(self):
        self.run_case('add_months')

    def test_october_28_2026_is_lunar_september_19(self):
        self.run_case('today')

    def test_lunar_year_changes_at_spring_festival(self):
        self.run_case('spring_festival')

    def test_leap_months_are_distinct_from_regular_months(self):
        self.run_case('leap_month')

    def test_terms_and_strictly_future_next_term(self):
        self.run_case('solar_terms')

    def test_next_term_crosses_the_gregorian_year(self):
        self.run_case('term_year_boundary')

    def test_table_boundaries_do_not_extrapolate(self):
        self.run_case('data_boundaries')


if __name__ == '__main__':
    unittest.main()
