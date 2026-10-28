"""Test real weight parser, missing days, latest sample, and unit conversion."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class WeightDataTests(unittest.TestCase):
    def test_real_parser(self):
        compiler=shutil.which('g++')
        if not compiler:self.skipTest('g++ unavailable')
        with tempfile.TemporaryDirectory(prefix='paperdesk-weight-') as tmp:
            exe=Path(tmp)/('weight.exe' if os.name=='nt' else 'weight')
            subprocess.run([compiler,'-std=c++17','-O2','-I'+str(ROOT/'firmware/main'),
                '-I'+str(ROOT/'firmware/components/json'),str(ROOT/'tests/weight_data_test.cpp'),
                str(ROOT/'firmware/components/json/cJSON.c'),'-o',str(exe)],check=True,timeout=60)
            subprocess.run([str(exe)],check=True,timeout=10)

if __name__=='__main__':unittest.main()
