import importlib.util, json, unittest
from pathlib import Path
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('bridge',Path(__file__).resolve().parents[1]/'bridge/server.py')
bridge=importlib.util.module_from_spec(spec);spec.loader.exec_module(bridge)

class QuotaTests(unittest.TestCase):
    def test_missing_is_not_zero(self):
        result=bridge.normalize_limits({'rateLimits':{'primary':None,'secondary':None}})
        self.assertFalse(result['ok']);self.assertEqual(result['windows'],[])
    def test_multi_bucket_wins_over_legacy_and_null_window(self):
        w={'usedPercent':21,'windowDurationMins':10080,'resetsAt':1793660829}
        result=bridge.normalize_limits({'rateLimits':{'primary':dict(w,usedPercent=99)},'rateLimitsByLimitId':{'codex':{'primary':w,'secondary':None},'other':{'primary':dict(w,usedPercent=100,windowDurationMins=300)}}})
        self.assertEqual([v['remaining'] for v in result['windows']],[79,0])
        self.assertEqual([v['label'] for v in result['windows']],['7天窗口','5小时窗口'])
    def test_zero_and_overflow(self):
        for used,want in [(0,100),(101,0),(-5,100)]:
            result=bridge.normalize_limits({'rateLimits':{'primary':{'usedPercent':used,'windowDurationMins':60}}})
            self.assertEqual(result['windows'][0]['remaining'],want)
    def test_invalid_percent_is_unavailable(self):
        for used in (None,True,float('nan'),float('inf')):
            result=bridge.normalize_limits({'rateLimits':{'primary':{'usedPercent':used}}})
            self.assertFalse(result['ok'])

class DataTests(unittest.TestCase):
    def test_config_never_echoes_secrets_or_inactive_fields(self):
        c={'token':'token-secret','weather':{'host':'test.qweatherapi.com','key':'weather-secret','location':'101020100','city':'上海'},'legacy_extra':{'secret':'inactive-secret'}}
        public=bridge.public_config(c)
        dumped=json.dumps(public)
        for secret in ['token-secret','weather-secret','inactive-secret']:self.assertNotIn(secret,dumped)
        self.assertEqual(set(public),{'weather'})
        self.assertTrue(public['weather']['key_set'])
        self.assertEqual(c['weather']['key'],'weather-secret')

    def test_update_preserves_blank_key_token_and_inactive_local_settings(self):
        previous={'token':'token-secret','weather':{'key':'weather-secret','city':'上海'},'legacy_extra':{'value':'retained locally'}}
        changed=bridge.update_config(previous,{'weather':{'key':'  ','city':' 青岛 '}})
        self.assertEqual(changed['weather']['key'],'weather-secret')
        self.assertEqual(changed['weather']['city'],'青岛')
        self.assertEqual(changed['token'],previous['token'])
        self.assertEqual(changed['legacy_extra'],previous['legacy_extra'])
        self.assertEqual(previous['weather']['city'],'上海')

    def test_invalid_config_is_rejected_without_mutating_saved_data(self):
        previous={'token':'keep','weather':{'key':'keep','city':'上海'}}
        for invalid in ([],{'unknown':{}},{'weather':[]},{'weather':{'extra':'x'}},{'weather':{'city':'长'*25}},{'weather':{'key':12}},{'weather':{'host':'bad\naddress'}}):
            with self.assertRaises(ValueError):bridge.update_config(previous,invalid)
        self.assertEqual(previous,{'token':'keep','weather':{'key':'keep','city':'上海'}})

    def test_weather_rejects_non_provider_host(self):
        with self.assertRaises(ValueError):bridge.weather_fetch({'weather':{'host':'evil.example','key':'anything'}})

    def test_weather_preserves_provider_icons_dates_and_sample_values(self):
        responses=[
            {'code':'200','updateTime':'2026-10-28T11:00+08:00','now':{'temp':'20','text':'晴','icon':'100','feelsLike':'19','windDir':'东北风','windScale':'2','humidity':'30'}},
            {'code':'200','daily':[{'fxDate':'2026-10-28','textDay':'晴','iconDay':'100','tempMin':'14','tempMax':'20'},{'fxDate':'2026-10-29','textDay':'小雨','iconDay':305,'tempMin':'13','tempMax':'18'}]}
        ]
        with patch.object(bridge,'http_json',side_effect=responses) as request:
            result=bridge.weather_fetch({'weather':{'host':'test.qweatherapi.com','key':'test','city':'青岛','location':'120.38,36.07'}})
        self.assertEqual(result['icon'],'100')
        self.assertEqual(result['temp'],'20');self.assertEqual(result['city'],'青岛')
        self.assertEqual(result['daily'][1],{'text':'小雨','icon':'305','date':'2026-10-29','min':'13','max':'18'})
        self.assertEqual(request.call_count,2)
        self.assertIn('location=120.38,36.07',request.call_args_list[0].args[0])
        self.assertEqual(request.call_args_list[0].kwargs['headers'],{'X-QW-Api-Key':'test'})

    def test_weather_missing_icon_and_date_remain_unknown(self):
        responses=[{'code':'200','now':{}},{'code':'200','daily':[{}]}]
        with patch.object(bridge,'http_json',side_effect=responses):
            result=bridge.weather_fetch({'weather':{'host':'test.qweatherapi.com','key':'test'}})
        self.assertEqual(result['icon'],'')
        self.assertEqual(result['daily'][0]['icon'],'');self.assertEqual(result['daily'][0]['date'],'')
        self.assertEqual(result['temp'],'--')


class FreshnessTests(unittest.TestCase):
    def run_two_refreshes(self,configs,weather_results):
        quota={'ok':True,'windows':[{'label':'7天窗口','used':24,'remaining':76}],'updated_at':1000}
        state={'schema':1,'weather':{'ok':False},'codex':{'ok':False}}
        with patch.object(bridge,'STATE',state), patch.object(bridge,'load_config',side_effect=configs), \
             patch.object(bridge.STOP,'is_set',side_effect=[False,False,True]), \
             patch.object(bridge.WAKE,'wait'), patch.object(bridge.WAKE,'clear'), \
             patch.object(bridge,'weather_fetch',side_effect=weather_results), \
             patch.object(bridge,'codex_limits',side_effect=[quota,RuntimeError('额度离线')]), \
             patch.object(bridge.time,'time',side_effect=[1000,1000,2801,2801]):
            bridge.refresh_loop()
        return state

    def test_provider_failures_keep_data_and_original_timestamps(self):
        config={'weather':{'host':'test.qweatherapi.com','key':'test','city':'青岛'}}
        weather={'ok':True,'city':'青岛','temp':'20','icon':'100','updated_at':1000}
        result=self.run_two_refreshes([config,config],[weather,RuntimeError('天气离线')])
        self.assertEqual(set(result),{'schema','generated_at','weather','codex'})
        self.assertTrue(result['weather']['stale']);self.assertEqual(result['weather']['updated_at'],1000)
        self.assertEqual(result['weather']['icon'],'100');self.assertEqual(result['weather']['error'],'天气离线')
        self.assertTrue(result['codex']['stale']);self.assertEqual(result['codex']['updated_at'],1000)
        self.assertEqual(result['codex']['windows'][0]['remaining'],76)
        self.assertEqual(result['generated_at'],2801)

    def test_location_change_does_not_relabel_old_weather(self):
        old={'weather':{'city':'上海','location':'101020100'}};new={'weather':{'city':'青岛','location':'101120201'}}
        result=self.run_two_refreshes([old,new],[{'ok':True,'city':'上海','temp':'20','updated_at':1000},RuntimeError('新地点查询失败')])
        self.assertFalse(result['weather']['ok']);self.assertNotIn('city',result['weather'])
        self.assertTrue(result['weather']['stale'])


if __name__=='__main__':unittest.main()
