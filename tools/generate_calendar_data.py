"""Build a compact offline lunar/solar-term table from Hong Kong Observatory data.

The firmware needs only the generated calendar_data.h, never network access.
Run with --download to fetch missing sources into ignored .local/calendar-cache.
Recorded SHA256 values pin cached/downloaded sources for reproducible rebuilding.
"""
from pathlib import Path
import argparse
import calendar
import concurrent.futures
import csv
import datetime as dt
import hashlib
import io
import json
import re
import time
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
CACHE=ROOT/'.local/calendar-cache'
MANIFEST=ROOT/'assets/calendar/source.json'
OUTPUT=ROOT/'firmware/main/calendar_data.h'
FIRST,LAST=1901,2100
EPOCH=dt.date(1900,1,1)
URL='https://www.hko.gov.hk/tc/gts/time/calendar/text/files/T{year}c.txt'
CSV_URL='https://data.weather.gov.hk/weatherAPI/hko_data/calendar/nongli_calendar_2026.csv'
MONTHS=['正月','二月','三月','四月','五月','六月','七月','八月','九月','十月','十一月','十二月']
DAYS=['初一','初二','初三','初四','初五','初六','初七','初八','初九','初十','十一','十二','十三','十四','十五','十六','十七','十八','十九','二十','廿一','廿二','廿三','廿四','廿五','廿六','廿七','廿八','廿九','三十']
TERMS=['小寒','大寒','立春','雨水','驚蟄','春分','清明','穀雨','立夏','小滿','芒種','夏至','小暑','大暑','立秋','處暑','白露','秋分','寒露','霜降','立冬','小雪','大雪','冬至']
SIMPLIFIED=['小寒','大寒','立春','雨水','惊蛰','春分','清明','谷雨','立夏','小满','芒种','夏至','小暑','大暑','立秋','处暑','白露','秋分','寒露','霜降','立冬','小雪','大雪','冬至']
WEEKDAYS='一二三四五六日'
LINE=re.compile(r'^(\d+)年(\d+)月(\d+)日\s+(\S+)\s+星期([一二三四五六日])(?:\s+(\S+))?\s*$')

def fetch(name,url,download,pinned):
    path=CACHE/name
    if not path.exists():
        if not download:raise FileNotFoundError(f'{path} missing; rerun with --download')
        last=None
        for attempt in range(3):
            try:
                opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
                request=urllib.request.Request(url,headers={'User-Agent':'Paperdesk-calendar-builder/1.0'})
                with opener.open(request,timeout=35) as response:data=response.read(128*1024+1)
                if len(data)>128*1024:raise ValueError(f'Source too large: {name}')
                path.write_bytes(data);break
            except Exception as error:
                last=error
                if attempt<2:time.sleep(0.5*(attempt+1))
        else:raise RuntimeError(f'Unable to download {name}: {last}')
    raw=path.read_bytes();sha=hashlib.sha256(raw).hexdigest()
    if name in pinned and sha!=pinned[name]:raise ValueError(f'Source checksum changed: {name}')
    return raw,{'file':name,'url':url,'sha256':sha}

def parse_year(year,raw):
    rows=[]
    for line in raw.decode('utf-8-sig').splitlines():
        match=LINE.match(line)
        if not match:continue
        yy,mm,dd,token,weekday,term=match.groups();date=dt.date(int(yy),int(mm),int(dd))
        assert date.year==year
        assert WEEKDAYS[date.weekday()]==weekday,(date,weekday)
        assert not term or term in TERMS,(date,term)
        assert token in DAYS or token.removeprefix('閏') in MONTHS,(date,token)
        rows.append((date,token,term))
    expected=[dt.date(year,1,1)+dt.timedelta(days=i) for i in range(366 if calendar.isleap(year) else 365)]
    assert [row[0] for row in rows]==expected,f'Noncontinuous dates in {year}'
    assert [row[2] for row in rows if row[2]]==TERMS,f'Solar-term ordering in {year}'
    return rows

def build(rows):
    starts=[];terms=[];decoded={}
    first_date,first_token,_=rows[0]
    # The first explicit month boundary is 1901-01-20 (twelfth month).
    # Backfill its preceding eleventh month; the non-leap 1900-11-11 anchor
    # was independently checked against 6tail/lunar-python at the revision
    # recorded in source.json. That verifier is not a build/runtime dependency.
    first_boundary=next((date,token) for date,token,_ in rows if token.removeprefix('閏') in MONTHS)
    assert first_boundary==(dt.date(1901,1,20),'十二月')
    lunar_year=first_date.year-1;month=MONTHS.index(first_boundary[1]);leap=False;day=DAYS.index(first_token)+1
    assert (first_date,month,day)==(dt.date(1901,1,1),11,11)
    start=first_date-dt.timedelta(days=day-1)
    starts.append((start,lunar_year,month,leap))
    previous=first_date-dt.timedelta(days=1)
    for date,token,term in rows:
        assert date==previous+dt.timedelta(days=1)
        if token.removeprefix('閏') in MONTHS:
            new_month=MONTHS.index(token.removeprefix('閏'))+1;new_leap=token.startswith('閏')
            assert (date-start).days in (29,30),(date,start)
            if new_leap:assert new_month==month and not leap,(date,token,month,leap)
            else:assert new_month==month%12+1,(date,token,month,leap)
            if new_month==1 and not new_leap:lunar_year+=1
            month,leap,start=new_month,new_leap,date;day=1
            starts.append((start,lunar_year,month,leap))
        else:
            day=(date-start).days+1
            assert 1<=day<=30 and token==DAYS[day-1],(date,token,day)
        decoded[date]=(lunar_year,month,day,leap)
        if term:terms.append((date,TERMS.index(term)))
        previous=date
    return starts,terms,decoded

def check_csv(raw,decoded):
    rows=list(csv.DictReader(io.StringIO(raw.decode('utf-8-sig'))))
    count=0
    for row in rows:
        date=dt.datetime.strptime(row['Gregorian Date'],'%d-%b-%y').date()
        month=row['Lunar month'];day=DAYS.index(row['Lunar Date'])+1
        actual=decoded[date]
        assert actual[1:]==(MONTHS.index(month.removeprefix('閏'))+1,day,month.startswith('閏')),(date,row,actual)
        stems='甲乙丙丁戊己庚辛壬癸';branches='子丑寅卯辰巳午未申酉戌亥'
        assert row['Chinese year (Gan-Zhi)']==stems[(actual[0]-4)%10]+branches[(actual[0]-4)%12]+'年'
        count+=1
    assert count==365
    return count

def packed_header(starts,terms):
    months=[]
    for date,year,month,leap in starts:
        ordinal=(date-EPOCH).days
        assert 0<=ordinal<1<<17 and 0<=year-1900<1<<8
        months.append(ordinal|(month<<17)|(int(leap)<<21)|((year-1900)<<22))
    solar=[(date-EPOCH).days|(index<<17) for date,index in terms]
    def array(name,values):
        lines=[f'inline constexpr uint32_t {name}[]={{']
        for i in range(0,len(values),8):lines.append('    '+','.join(f'0x{v:08x}u' for v in values[i:i+8])+',')
        return '\n'.join(lines)+'\n};\n'
    text='// Generated by tools/generate_calendar_data.py from HKO date facts.\n// Source URLs and SHA256: assets/calendar/source.json. Do not edit by hand.\n#pragma once\n#include <cstdint>\nnamespace paperdesk_calendar::data {\n'
    text+=f'inline constexpr int first_day={(dt.date(FIRST,1,1)-EPOCH).days};\ninline constexpr int last_day={(dt.date(LAST,12,31)-EPOCH).days};\n'
    text+='inline constexpr const char* term_names[]={'+','.join('"'+s+'"' for s in SIMPLIFIED)+'};\n'
    text+=array('month_starts',months)+array('terms',solar)+'} // namespace paperdesk_calendar::data\n'
    OUTPUT.write_text(text,encoding='utf-8')
    return 4*(len(months)+len(solar))

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--download',action='store_true');args=parser.parse_args()
    CACHE.mkdir(parents=True,exist_ok=True)
    pinned={}
    if MANIFEST.exists():pinned={item['file']:item['sha256'] for item in json.loads(MANIFEST.read_text(encoding='utf-8'))['sources']}
    def year_job(year):
        raw,source=fetch(f'T{year}c.txt',URL.format(year=year),args.download,pinned)
        return year,parse_year(year,raw),source
    years={};sources=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
        futures=[executor.submit(year_job,year) for year in range(FIRST,LAST+1)]
        for future in concurrent.futures.as_completed(futures):
            year,rows,source=future.result();years[year]=rows;sources.append(source)
            if len(years)%20==0:print(f'Validated {len(years)}/200 official year files',flush=True)
    rows=[row for year in sorted(years) for row in years[year]]
    starts,terms,decoded=build(rows)
    csv_raw,source=fetch('nongli_calendar_2026.csv',CSV_URL,args.download,pinned);sources.append(source)
    count=check_csv(csv_raw,decoded)
    data_bytes=packed_header(starts,terms)
    manifest={'provider':'Hong Kong Observatory','source_index':'https://www.hko.gov.hk/sc/gts/time/conversion1_text.htm','supported_gregorian_start':'1901-01-01','supported_gregorian_end':'2100-12-31','content':'Gregorian/lunar date and solar-term date facts; no forecasts or holiday/workday schedules','encoding':'uint32 month starts and solar-term dates; day offset from 1900-01-01','first_lunar_anchor':{'gregorian':'1901-01-01','lunar_year':1900,'lunar_month':11,'lunar_day':11,'leap':False,'method':'Backfill before the first explicit HKO month boundary, 1901-01-20 twelfth month; independently checked with Solar.fromYmd(1901,1,1).getLunar()','independent_verifier':'https://github.com/6tail/lunar-python/tree/448f397c1695cadab3899bf460e0042cab7f0e66','verifier_is_build_or_runtime_dependency':False},'checks':{'gregorian_days':len(rows),'lunar_month_starts':len(starts),'solar_terms':len(terms),'independent_2026_csv_days':count,'packed_bytes':data_bytes},'sources':sorted(sources,key=lambda item:item['file'])}
    MANIFEST.parent.mkdir(parents=True,exist_ok=True);MANIFEST.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(manifest['checks']),flush=True)

if __name__=='__main__':main()
