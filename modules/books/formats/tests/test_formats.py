"""Content, resource, cancellation and allocation-failure integration tests."""
from pathlib import Path
import re
import subprocess
import zipfile

root = Path(__file__).resolve().parent
corpus = root / "build/corpus"
corpus.mkdir(exist_ok=True)
exe = root / "build/book_formats_test"
png = b"\x89PNG\r\n\x1a\n" + bytes(range(256)) * 31
container = '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'

def archive(path, files, stored=False):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)

def run(path, expected=0, fail=0, cancel=0, image_result=0):
    p = subprocess.run([str(exe), str(path), str(fail), str(cancel), str(image_result)], text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    assert "live=0" in p.stdout, (path, fail, p.returncode, p.stdout[-1000:], p.stderr[-1000:])
    result = int(re.search(r"RESULT (-?\d+)", p.stdout)[1])
    assert p.returncode == (0 if result == 0 else 1), (path, fail, p.returncode, p.stderr[-2000:])
    assert not p.stderr.strip(), (path, fail, p.stderr[-4000:])
    if expected is not None:
        assert result == expected, (path, fail, result, p.stdout[-2000:], p.stderr[-1000:])
    return p.stdout, result

def repeat(path, fail=0, cancel=0):
    p = subprocess.run([str(exe), str(path), str(fail), str(cancel), '0', '3'], text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    results = re.findall(r"RESULT (-?\d+) live=(\d+)", p.stdout)
    first = -3 if fail else -2 if cancel else 0
    assert p.returncode == 0 and not p.stderr.strip(), (path, p.returncode, p.stderr[-4000:])
    assert results == [(str(first), '0'), ('0', '0'), ('0', '0')], (path, results)

opf = '<package xmlns="http://www.idpf.org/2007/opf" xmlns:d="http://purl.org/dc/elements/1.1/" version="3.0"><metadata><d:title>EPUB 中文</d:title><d:creator>Author</d:creator></metadata><manifest><item id="second" href="two.xhtml" media-type="application/xhtml+xml"/><item id="first" href="text/one.xhtml" media-type="application/xhtml+xml"/><item id="toc" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest><spine><itemref idref="first"/><itemref idref="second"/></spine></package>'
one = '<html xmlns="http://www.w3.org/1999/xhtml"><head><link rel="stylesheet" href="../style.css"/></head><body><h1 id="start">Chapter One</h1><p class="em">First <b>bold</b> tail <a href="../two.xhtml#end">next</a></p><img src="../img/cover%20image.png"/><table><tr><td>A</td><td>B</td></tr></table></body></html>'
two = '<html xmlns="http://www.w3.org/1999/xhtml"><body><h2 id="end">Chapter Two</h2><p>Second body</p></body></html>'
nav = '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:e="http://www.idpf.org/2007/ops"><body><nav e:type="toc"><ol><li><a href="text/one.xhtml#start">First TOC</a><ol><li><a href="two.xhtml#end">Second TOC</a></li></ol></li></ol></nav></body></html>'
files = {'META-INF/container.xml': container, 'OPS/book.opf': opf, 'OPS/text/one.xhtml': one,
         'OPS/two.xhtml': two, 'OPS/nav.xhtml': nav, 'OPS/style.css': '.em {font-style:italic}', 'OPS/img/cover image.png': png}
epub = corpus / 'sample.epub'
archive(epub, files)
out, _ = run(epub)
assert out.index('Chapter One') < out.index('Chapter Two')
assert 'ANCHOR 2 OPS/two.xhtml#end | Second TOC' in out
assert f'IMAGE OPS/img/cover image.png {len(png)} ' in out
assert 'LINK 1 OPS/two.xhtml#end' in out and 'TEXT 2 First' in out

# Tail selectors must survive the former rule, inline and external CSS caps.
css_book = corpus / 'css-growth.epub'
long_class = 'selector' + 'x' * 120
rules = ''.join(f'.unused{i} {{font-weight:normal}}\n' for i in range(200))
external_css = rules + ' ' * 70000 + f'.{long_class} {{font-style:italic}}'
inline_css = ' ' * 20000 + '.inline-tail {font-weight:bold}'
css_page = ('<html xmlns="http://www.w3.org/1999/xhtml"><head>'
            '<link rel="stylesheet" href="../style.css"/>'
            f'<style>{inline_css}</style></head><body>'
            f'<p class="{long_class}">EXTERNAL_TAIL</p>'
            '<p class="inline-tail">INLINE_TAIL</p></body></html>')
archive(css_book, {**files, 'OPS/style.css': external_css, 'OPS/text/one.xhtml': css_page})
out, _ = run(css_book)
assert 'TEXT 2 EXTERNAL_TAIL' in out and 'TEXT 1 INLINE_TAIL' in out
# Every allocator failure in this fixture must propagate and release its scope.
for fail in range(1, int(re.search(r'allocations=(\d+)', out)[1]) + 1):
    _, result = run(css_book, None, fail=fail)
    assert result in (-3, 0), (css_book, fail, result)
repeat(css_book, cancel=3)

ncxfiles = dict(files)
ncxfiles['OPS/book.opf'] = opf.replace('version="3.0"', 'version="2.0"').replace('href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"', 'href="toc.ncx" media-type="application/x-dtbncx+xml"').replace('<spine>', '<spine toc="toc">')
ncxfiles['OPS/toc.ncx'] = '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap><navPoint id="a"><navLabel><text>NCX One</text></navLabel><content src="text/one.xhtml#start"/><navPoint id="b"><navLabel><text>NCX Child</text></navLabel><content src="two.xhtml#end"/></navPoint></navPoint></navMap></ncx>'
epub2 = corpus / 'epub2.epub'
archive(epub2, ncxfiles, stored=True)
out, _ = run(epub2)
assert 'ANCHOR 2 OPS/two.xhtml#end | NCX Child' in out

split = corpus / 'split'
split.mkdir(exist_ok=True)
splitfiles = dict(files)
splitfiles['OPS/book.opf'] = opf.replace('<spine><itemref idref="first"/><itemref idref="second"/></spine>', '<spine/>')
splitfiles.pop('OPS/text/one.xhtml'); splitfiles.pop('OPS/two.xhtml')
archive(split / 'basepackage.zip', splitfiles)
for number in (10, 2, 1):
    archive(split / f'chapter{number}.zip', {f'OPS/ch{number}.xhtml': f'<html><body><p>PART{number}</p><img src="img/cover%20image.png"/></body></html>'})
out, _ = run(split)
assert out.index('PART1\n') < out.index('PART2\n') < out.index('PART10\n')
assert 'chapter2.zip!OPS/ch2.xhtml' in out

md = corpus / 'sample.md'
(corpus / 'picture.png').write_bytes(png)
md.write_text('# MD Title\nText **bold** and [link](#md-title).\n![Image](picture.png)\n| A | B |\n|---|---|\n| 1 | 2 |\n```\n  code\n```\n' + '中文' * 20000, encoding='utf-8')
out, _ = run(md)
assert 'MD Title' in out and 'TEXT 1 bold' in out and f'picture.png {len(png)} ' in out
rendered = ''.join(re.findall(r'^TEXT \d+ (.*)$', out, re.M))
assert '| 1 | 2 |' in out and 'TEXT 4   code' in out and rendered.count('中文') == 20000

large = corpus / 'large.epub'
archive(large, {**files, 'OPS/two.xhtml': two.replace('Second body', 'Large正文 ' * 250000)})
out, _ = run(large)
rendered = ''.join(re.findall(r'^TEXT \d+ (.*)$', out, re.M))
assert rendered.count('Large正文') == 250000
large_peak = int(re.search(r'peak=(\d+)', out)[1])

bad = corpus / 'bad.epub'
archive(bad, {**files, 'OPS/book.opf': '<broken>'})
run(bad, -1)
entity = corpus / 'entity.epub'
archive(entity, {**files, 'OPS/book.opf':
    '<!DOCTYPE package [<!ENTITY x SYSTEM "file:///etc/passwd">]>'
    + opf.replace('EPUB 中文', '&x;')})
run(entity, -4)

crc_bad = corpus / 'crc.epub'
data = bytearray(epub2.read_bytes())
pos = 0
while True:
    pos = data.find(b'PK\x01\x02', pos)
    if pos < 0:
        raise AssertionError('PNG central directory entry not found')
    size = int.from_bytes(data[pos + 28:pos + 30], 'little')
    if data[pos + 46:pos + 46 + size] == b'OPS/img/cover image.png':
        data[pos + 16] ^= 1
        break
    pos += 4
crc_bad.write_bytes(data)
run(crc_bad, -1, image_result=1)
run(epub, image_result=1)
for path in (epub, md):
    run(path, -1, image_result=-1)
    run(path, -4, image_result=-4)
svg = corpus / 'svg.md'
svg.write_text('![vector](drawing.svg)')
run(svg, -4)

for path in (epub, epub2, split, md):
    repeat(path)
    repeat(path, fail=1)
    repeat(path, cancel=3)
    run(path, -2, cancel=3)
    _, _ = run(path)
    for fail in list(range(1, 40)) + [50, 70, 100, 150, 200, 300, 500]:
        _, result = run(path, None, fail=fail)
        assert result in (-3, 0), (path, fail, result)
print('PASS: EPUB2/3, CSS growth/tail styles/OOM, split natural order/resource fallback, Markdown, large text, malformed/entity rejection, partial-image CRC, image errors, repeated scopes, cancellation, OOM cleanup')
assert large_peak < 1024 * 1024, f'Large continuous-text peak exceeds the existing 1 MiB test budget: {large_peak}'
