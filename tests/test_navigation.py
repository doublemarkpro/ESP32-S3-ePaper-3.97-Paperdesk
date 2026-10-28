"""Exercise the actual firmware key handler and rotation branch on the host.

Only hardware services are stubbed. The production handler and rotation branch
are extracted so these regressions cannot silently test a second implementation.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def braced_statement(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == '{':
            depth += 1
        elif source[position] == '}':
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise AssertionError(f'Unclosed production statement: {marker}')


HARNESS = r'''
#include "paperdesk.h"
#include "gallery.h"
#include "garmin.h"
#include <cassert>
#include <cstdint>
#include <iostream>
AppData app;std::mutex app_mutex;
std::atomic<bool> dirty{false},force_sync{false},full_refresh{false},weather_full_refresh{false},calendar_full_refresh{false};
int saves=0,requests=0,garmin_requests=0;
void config_save(){++saves;}
namespace paperdesk_gallery {std::atomic<bool> full_refresh{false};void request_next(){++requests;}}
namespace paperdesk_garmin {void request_sync(){++garmin_requests;}}
PRODUCTION_HANDLER
void rotate(View&view,bool enabled=true,bool quiet=false,int64_t elapsed=300000000){
    Config cfg;cfg.rotation_minutes=enabled?5:0;int64_t now=elapsed,last_interaction=0;
    PRODUCTION_ROTATION
}
int main(int argc,char**argv){
    assert(argc==2);std::string test=argv[1];
    if(test=="content_pages"){
        for(int p=HomePage;p<ContentPageCount;++p){
            View down{p};handle_key(down,Down);assert(down.page==(p+1)%ContentPageCount);assert(down.page!=SettingsPage);
            View up{p};handle_key(up,Up);assert(up.page==(p+ContentPageCount-1)%ContentPageCount);assert(up.page!=SettingsPage);
            View automatic{p};rotate(automatic);assert(automatic.page==(p+1)%ContentPageCount);assert(automatic.page!=SettingsPage);
        }
    }else if(test=="settings_exclusion"){
        for(int p=HomePage;p<ContentPageCount;++p){
            View view{p};handle_key(view,Back);assert(view.page==SettingsPage);
            rotate(view);assert(view.page==SettingsPage);
            handle_key(view,Back);assert(view.page==HomePage);
        }
        View settings{SettingsPage};settings.selection=6;handle_key(settings,Down);
        assert(settings.selection==0&&settings.page==SettingsPage);
        settings.details=true;handle_key(settings,Up);assert(settings.selection==0&&settings.page==SettingsPage);
    }else if(test=="rotation_gates"){
        for(int p=HomePage;p<ContentPageCount;++p){
            View view{p};rotate(view,false);assert(view.page==p);
            rotate(view,true,true);assert(view.page==p);
            rotate(view,true,false,299999999);assert(view.page==p);
            rotate(view,true,false,300000000);assert(view.page==(p+1)%ContentPageCount);
        }
    }else if(test=="page_actions"){
        View gallery{GalleryPage};handle_key(gallery,Press);
        assert(requests==1&&!force_sync&&gallery.page==GalleryPage);
        View garmin{GarminPage};handle_key(garmin,Press);
        assert(garmin_requests==1&&!force_sync&&garmin.page==GarminPage);
        View calendar{CalendarPage};handle_key(calendar,Press);assert(calendar.selection==1&&calendar_full_refresh);
        handle_key(calendar,Press);assert(calendar.selection==0&&!force_sync);
        View weather{WeatherPage};handle_key(weather,Press);assert(force_sync);
    }else if(test=="startup_pages"){
        View settings{SettingsPage};settings.selection=3;app.config.default_page=HomePage;
        for(int i=1;i<=ContentPageCount;++i){handle_key(settings,Press);assert(app.config.default_page==i%ContentPageCount);}
        assert(saves==ContentPageCount);
    }else if(test=="gallery_refresh"){
        View view{CalendarPage};handle_key(view,Down);assert(view.page==GalleryPage&&paperdesk_gallery::full_refresh);
        paperdesk_gallery::full_refresh=false;view.page=CalendarPage;rotate(view);
        assert(view.page==GalleryPage&&paperdesk_gallery::full_refresh);
    }else return 2;
    std::cout<<test<<" passed\n";
}
'''


class NavigationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('g++ or clang++ is required for firmware navigation tests')
        source = (ROOT / 'firmware/main/app.cpp').read_text(encoding='utf-8')
        test = HARNESS.replace('PRODUCTION_HANDLER', braced_statement(source, 'static void handle_key'))
        test = test.replace('PRODUCTION_ROTATION', braced_statement(source, 'if(cfg.rotation_minutes&&'))
        temporary = tempfile.TemporaryDirectory(prefix='paperdesk-navigation-test-')
        cls.addClassCleanup(temporary.cleanup)
        source_file = Path(temporary.name) / 'navigation.cpp'
        source_file.write_text(test, encoding='utf-8')
        cls.executable = Path(temporary.name) / ('navigation.exe' if os.name == 'nt' else 'navigation')
        result = subprocess.run(
            [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-DPAPERDESK_HOST',
             '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
             '-I', str(ROOT / 'firmware/main'), '-I', str(ROOT / 'firmware/components/json'),
             str(source_file), '-o', str(cls.executable)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
        if result.returncode:
            raise AssertionError(f'Navigation test compilation failed:\n{result.stdout}\n{result.stderr}')

    def run_case(self, case):
        result = subprocess.run([str(self.executable), case], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=10)
        self.assertEqual(result.returncode, 0, f'{case}:\n{result.stdout}\n{result.stderr}')

    def test_manual_and_automatic_navigation_cover_only_content_pages(self):
        self.run_case('content_pages')

    def test_settings_never_participates_in_rotation(self):
        self.run_case('settings_exclusion')

    def test_rotation_respects_delay_quiet_hours_and_disabled_mode(self):
        self.run_case('rotation_gates')

    def test_short_press_keeps_gallery_calendar_and_gateway_actions_separate(self):
        self.run_case('page_actions')

    def test_default_page_selector_cycles_all_content_pages(self):
        self.run_case('startup_pages')

    def test_entering_gallery_requests_a_full_refresh(self):
        self.run_case('gallery_refresh')


if __name__ == '__main__':
    unittest.main()
