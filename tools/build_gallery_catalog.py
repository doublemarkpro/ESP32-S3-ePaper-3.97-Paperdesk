"""Generate the firmware's curated Met Open Access artwork directory.

Normal generation is offline and needs only the tracked JSON manifest. Optional
--verify checks the original metadata and baseline JPEG files in the ignored
cache; --download fetches missing originals directly from the official hosts.
Pillow is needed only for verification. No image data is embedded in firmware.
"""
from pathlib import Path
import argparse
import hashlib
import json
import time
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'assets/gallery/catalog.json'
OUT = ROOT / 'firmware/main/gallery_catalog.h'
CACHE = ROOT / '.local/gallery-source'
MAX_JPEG_BYTES = 768 * 1024


def official_url(value, host):
    url = urllib.parse.urlsplit(value)
    return (url.scheme == 'https' and url.hostname == host and not url.username
            and not url.password and not url.fragment and url.port in (None, 443))


def validate_manifest(manifest):
    if manifest.get('schema') != 1 or not manifest.get('artworks'):
        raise ValueError('Missing gallery schema or artworks')
    seen = set()
    for item in manifest['artworks']:
        oid = item['object_id']
        if type(oid) is not int or oid <= 0 or oid in seen:
            raise ValueError('Invalid or duplicate artwork ID')
        seen.add(oid)
        if item.get('is_public_domain') is not True:
            raise ValueError(f'{oid}: public domain verification is required')
        for key, host in [('image_url', 'images.metmuseum.org'),
                          ('source_url', 'www.metmuseum.org'),
                          ('metadata_url', 'collectionapi.metmuseum.org')]:
            if not official_url(item[key], host):
                raise ValueError(f'{oid}: unexpected {key} host')
        for key in ('title', 'artist', 'date'):
            if not isinstance(item[key], str) or not item[key] or len(item[key]) > 40:
                raise ValueError(f'{oid}: invalid display {key}')
        image = item['image']
        if image['format'] != 'JPEG' or image['progressive']:
            raise ValueError(f'{oid}: only baseline JPEG is supported')
        if not 0 < image['bytes'] <= MAX_JPEG_BYTES:
            raise ValueError(f'{oid}: oversized JPEG')
        if not all(0 < image[k] <= 4096 for k in ('width', 'height')):
            raise ValueError(f'{oid}: invalid JPEG dimensions')


def verify(manifest, download):
    from PIL import Image
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    CACHE.mkdir(parents=True, exist_ok=True)
    for item in manifest['artworks']:
        oid = item['object_id']
        for ext, url, checksum, limit in [
            ('json', item['metadata_url'], item['metadata_sha256'], 128 * 1024),
            ('jpg', item['image_url'], item['image']['sha256'], MAX_JPEG_BYTES),
        ]:
            path = CACHE / f'{oid}.{ext}'
            if not path.exists() and download:
                request = urllib.request.Request(url, headers={
                    'User-Agent': 'Paperdesk-gallery-catalog/0.6.0',
                    'Accept-Encoding': 'identity'})
                with opener.open(request, timeout=30) as response:
                    data = response.read(limit + 1)
                    if response.geturl() != url:
                        raise ValueError(f'{oid}: unexpected download redirect')
                if len(data) > limit:
                    raise ValueError(f'{oid}: download exceeds size limit')
                if hashlib.sha256(data).hexdigest() != checksum:
                    raise ValueError(f'{oid}: source changed; review before updating manifest')
                path.write_bytes(data)
                time.sleep(1)
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != checksum:
                raise ValueError(f'{oid}: source checksum mismatch')
        meta = json.loads((CACHE / f'{oid}.json').read_bytes())
        if (meta['objectID'] != oid or meta['isPublicDomain'] is not True
                or meta['primaryImageSmall'] != item['image_url']
                or meta['objectURL'] != item['source_url']):
            raise ValueError(f'{oid}: official metadata does not match manifest')
        for field, original in [('title', 'title'), ('artistDisplayName', 'artist'),
                                ('objectDate', 'date')]:
            if meta[field] != item['original'][original]:
                raise ValueError(f'{oid}: original {field} does not match')
        with Image.open(CACHE / f'{oid}.jpg') as image:
            image.load()
            if (image.format != 'JPEG' or image.info.get('progressive')
                    or image.info.get('progression')
                    or image.size != (item['image']['width'], item['image']['height'])):
                raise ValueError(f'{oid}: JPEG verification failed')
    print(f"Verified {len(manifest['artworks'])} official metadata and baseline JPEG pairs")


def generate(manifest):
    quoted = lambda value: json.dumps(value, ensure_ascii=False)
    lines = [
        '// Generated by tools/build_gallery_catalog.py; do not edit.',
        '// Images are fetched directly from The Met; no artwork JPEG is embedded.',
        '// Source facts, public-domain checks and hashes: assets/gallery/catalog.json.',
        '#pragma once', '#include <cstddef>', '',
        'namespace paperdesk_gallery {',
        'struct Artwork {',
        '    int object_id;',
        '    const char* title;',
        '    const char* artist;',
        '    const char* date;',
        '    const char* image_url;',
        '    const char* source_url;',
        '};',
        'inline constexpr Artwork artworks[] = {',
    ]
    for item in manifest['artworks']:
        values = [str(item['object_id'])] + [quoted(item[key]) for key in (
            'title', 'artist', 'date', 'image_url', 'source_url')]
        lines.append('    {' + ', '.join(values) + '},')
    lines += ['};', 'inline constexpr size_t artwork_count = sizeof(artworks) / sizeof(artworks[0]);',
              '} // namespace paperdesk_gallery', '']
    OUT.write_text('\n'.join(lines), encoding='utf-8', newline='\n')
    print(f"Generated directory with {len(manifest['artworks'])} artworks")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--download', action='store_true', help='Download missing originals and verify them')
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    validate_manifest(manifest)
    if args.verify or args.download:
        verify(manifest, args.download)
    generate(manifest)


if __name__ == '__main__':
    main()
