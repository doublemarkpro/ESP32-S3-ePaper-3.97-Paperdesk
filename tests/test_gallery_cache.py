"""Check the real persistent gallery record, including corrupt/torn NVS data."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class GalleryCacheTests(unittest.TestCase):
    def test_round_trip_and_corrupt_records_do_not_replace_current_picture(self):
        compiler=shutil.which('g++')
        if not compiler:self.skipTest('g++ required')
        source=r'''
#include "gallery_cache.h"
#include <cassert>
int main(){
    gallery_cache::Record source;source.object_id=362554;source.day=20261028;source.updated_at=1793160000;
    source.pixels.resize(gallery_image::Bytes);
    for(size_t i=0;i<source.pixels.size();++i)source.pixels[i]=uint8_t(i*17);
    auto saved=gallery_cache::encode(source);assert(saved.size()==gallery_cache::Size);
    gallery_cache::Record loaded;assert(gallery_cache::decode(saved,loaded));
    assert(loaded.object_id==source.object_id&&loaded.day==source.day&&loaded.updated_at==source.updated_at&&loaded.pixels==source.pixels);
    for(size_t position:{size_t(0),size_t(4),size_t(8),size_t(12),size_t(20),size_t(100),saved.size()-1}){
        auto bad=saved;bad[position]^=0x80;assert(!gallery_cache::decode(bad,loaded));assert(loaded.pixels==source.pixels);
    }
    for(size_t length:{size_t(0),size_t(4),size_t(24),saved.size()-1}){
        auto bad=saved;bad.resize(length);assert(!gallery_cache::decode(bad,loaded));assert(loaded.object_id==source.object_id);
    }
    auto bad=saved;bad.push_back(0);assert(!gallery_cache::decode(bad,loaded));
    auto invalid=source;invalid.pixels.pop_back();assert(gallery_cache::encode(invalid).empty());
    invalid=source;invalid.object_id=0;assert(gallery_cache::encode(invalid).empty());
    invalid=source;invalid.updated_at=0;assert(gallery_cache::encode(invalid).empty());
}
'''
        with tempfile.TemporaryDirectory(prefix='paperdesk-cache-test-') as folder:
            folder=Path(folder);(folder/'test.cpp').write_text(source)
            executable=folder/'test.exe'
            subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'firmware/main'),str(folder/'test.cpp'),'-o',str(executable)],check=True,capture_output=True)
            subprocess.run([str(executable)],check=True,capture_output=True)

if __name__=='__main__':unittest.main()
