"""Generator + production C reader tests for WDB2, with real-source validation support."""
import argparse
import ctypes as C
import csv
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import unified_dictionary as builder

READ = C.CFUNCTYPE(C.c_bool, C.c_void_p, C.c_uint32, C.c_void_p, C.c_size_t)
class Dictionary(C.Structure):
    _fields_ = [('read', READ), ('context', C.c_void_p)] + [(n, C.c_uint32) for n in
        ('count', 'aliases', 'index', 'alias_index', 'data', 'size', 'version', 'scopes', 'scope_index', 'members', 'data_end')] + [('identity', C.c_ubyte * 24)]
class Entry(C.Structure):
    _fields_ = [('word', C.c_char_p), ('legacy', C.c_char_p * 6), ('records', C.c_void_p)] + [(n, C.c_uint32) for n in ('size', 'count', 'version')]
class Scope(C.Structure):
    _fields_ = [('id', C.c_char * 32), ('name', C.c_char * 64), ('first', C.c_uint32), ('count', C.c_uint32)]

def fixture(folder, extra=False):
    source = folder / 'source.csv'
    with source.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.writer(stream); writer.writerow(('word', 'phonetic', 'translation', 'definition', 'exchange', 'tag'))
        for word in (['!added'] if extra else []) + ['Apple', 'apple'] + [f'word{i:03}' for i in range(80)]:
            writer.writerow((word, 'æpl', 'n. 测试释义', 'A test entry.', 's:apples' if word == 'apple' else '', 'cet4 cet6' if word in ('Apple', 'apple') else 'cet4'))
        writer.writerow(('apple', '', '额外释义', '', '', 'cet4'))
        writer.writerow(('x' * 96, '', '超长词头', '', '', ''))
    catalog = folder / 'catalog.json'
    catalog.write_text(json.dumps({'data': {'normalBooksInfo': [
        {'id': 'CET4_3', 'title': '新东方四级词汇', 'version': '3', 'tags': [{'tagName': '四级'}]},
        {'id': 'CET6_3', 'title': '新东方六级词汇', 'version': '2', 'tags': [{'tagName': '六级'}]}]}}, ensure_ascii=False), encoding='utf-8')
    files = []
    for book in ('CET4_3', 'CET6_3'):
        path = folder / (book + '.json'); files.append(path)
        rows = []
        for word in ('Apple', 'apple'):
            rows.append({'headWord': word, 'bookId': book, 'content': {'word': {'wordHead': word,
                'wordId': book + '_' + word, 'content': {
                    'usphone': 'ˈæpəl', 'ukphone': 'ˈæpl', 'trans': [{'pos': 'n', 'tranCn': '苹果', 'tranOther': 'A fruit.'}],
                    'sentence': {'sentences': [{'sContent': 'I eat an apple.', 'sCn': '我吃一个苹果。'}] * 2},
                    'phrase': {'phrases': [{'pContent': 'apple tree', 'pCn': '苹果树'}]},
                    'relWord': {'rels': [{'pos': 'n', 'words': [{'hwd': 'applet', 'tran': '小程序'}]}]}
                }}}})
        path.write_text('\n'.join(json.dumps(row, ensure_ascii=False) for row in rows) + '\n{invalid', encoding='utf-8')
    path = folder / 'unified.wdb'
    return path, builder.build(source, path, files, catalog)

class Tests(unittest.TestCase):
    def inputs(self, root):
        source = root / 'source.csv'
        source.write_text('word,translation,tag\napple,base,cet4\n', encoding='utf-8')
        catalog = root / 'catalog.json'
        catalog.write_text(json.dumps({'data': {'normalBooksInfo': [
            {'id': 'CET4_3', 'title': '四级', 'version': '3', 'tags': [{'tagName': '四级'}]}]}}), encoding='utf-8')
        row = {'headWord': 'apple', 'bookId': 'CET4_3', 'content': {'word': {
            'wordHead': 'apple', 'wordId': 'CET4_3_1', 'content': {
                'trans': [{'pos': 'n', 'tranCn': '补充释义', 'tranOther': 'supplement'}]}}}}
        return source, catalog, row

    def write_rows(self, path, rows):
        path.write_text('\n'.join(json.dumps(row, ensure_ascii=True) for row in rows), encoding='utf-8')

    def contents(self, path):
        ok, d, read = self.open(path.read_bytes()); self.assertTrue(ok)
        return {self.entry(d, i)[0]: self.entry(d, i)[1] for i in range(d.count)}

    def open(self, blob, fail=False):
        @READ
        def read(ctx, offset, buffer, size):
            if fail or offset + size > len(blob): return False
            C.memmove(buffer, bytes(blob[offset:offset + size]), size); return True
        d = Dictionary()
        return lib.words_dictionary_open(C.byref(d), read, None, len(blob)), d, read

    def entry(self, d, index):
        size = C.c_uint32()
        self.assertTrue(lib.words_dictionary_entry_size(C.byref(d), index, C.byref(size)))
        buffer, e = C.create_string_buffer(size.value), Entry()
        self.assertTrue(lib.words_dictionary_read_entry(C.byref(d), index, buffer, size.value, C.byref(e)))
        required = lib.words_entry_text(C.byref(e), 2, None, 0)
        text = C.create_string_buffer(required)
        self.assertEqual(required, lib.words_entry_text(C.byref(e), 2, text, required))
        return e.word.decode(), text.value.decode(), buffer, e

    def test_merge_scopes_and_reorder(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); a = root / 'a'; b = root / 'b'; a.mkdir(); b.mkdir()
            path, report = fixture(a); newer, _ = fixture(b, extra=True)
            blob = path.read_bytes()
            ok, d, read = self.open(blob); self.assertTrue(ok)
            words = [self.entry(d, i)[0] for i in range(d.count)]
            self.assertEqual(words.count('apple'), 1); self.assertIn('Apple', words)
            _, text, _, _ = self.entry(d, words.index('apple'))
            for value in ('额外释义', '苹果树', 'I eat an apple.', 'ˈæpəl', 'apples', 'CET4_3_apple', 'CET6_3_apple'):
                self.assertIn(value, text)
            self.assertEqual(text.count('I eat an apple.'), 1)
            self.assertEqual(text.count('apple tree'), 1)
            self.assertIn('kajweb:CET4_3,kajweb:CET6_3', text)
            self.assertEqual(len(report['rejected']), 3)
            scopes = {}
            for i in range(d.scopes):
                scope = Scope(); self.assertTrue(lib.words_dictionary_scope(C.byref(d), i, C.byref(scope)))
                ids = []
                for pos in range(scope.count):
                    idx = C.c_uint32(); self.assertTrue(lib.words_dictionary_member(C.byref(d), C.byref(scope), pos, C.byref(idx)))
                    ids.append(idx.value)
                self.assertEqual(ids, sorted(set(ids))); scopes[scope.id.decode()] = ids
            self.assertEqual(len(scopes['cet6']), 2)
            ok, updated, callback = self.open(newer.read_bytes()); self.assertTrue(ok)
            for dictionary in (d, updated):
                index, found = C.c_uint32(), C.c_bool()
                self.assertTrue(lib.words_dictionary_find(C.byref(dictionary), b'apple', C.byref(index), C.byref(found)))
                self.assertTrue(found.value); self.assertEqual(self.entry(dictionary, index.value)[0], 'apple')
            self.assertEqual(report['sources'][0]['license'], 'MIT')
            self.assertEqual(report['sources'][1]['license'], 'unspecified')

    def test_corruption_and_read_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            path, _ = fixture(Path(temporary)); blob = path.read_bytes()
            self.assertFalse(self.open(blob, fail=True)[0])
            for offset in (8, 12, 16, 20, 24, 28, 32, 36, 64, 68, 72, 76, 80, 84):
                bad = bytearray(blob); struct.pack_into('<I', bad, offset, 0xffffffff)
                self.assertFalse(self.open(bad)[0], offset)
            for offset, value in ((96 + 96, len(blob) + 1), (96 + 100, 0xffffffff)):
                bad = bytearray(blob); struct.pack_into('<I', bad, offset, value)
                ok, d, read = self.open(bad); self.assertTrue(ok)
                size = C.c_uint32(); self.assertFalse(lib.words_dictionary_entry_size(C.byref(d), 0, C.byref(size)))
            ok, d, read = self.open(blob); self.assertTrue(ok)
            word, text, buffer, e = self.entry(d, 0)
            invalid = bytearray(buffer.raw); invalid[-1] = 255
            view = Entry(); raw = C.create_string_buffer(bytes(invalid))
            self.assertFalse(lib.words_entry_parse(raw, len(invalid), C.byref(view)))
            mutable = bytearray(blob); ok, d, read = self.open(mutable)
            self.assertTrue(lib.words_dictionary_unchanged(C.byref(d)))
            mutable[40] ^= 1; self.assertFalse(lib.words_dictionary_unchanged(C.byref(d)))

    def test_large_entry_and_determinism(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); source = root / 'large.csv'
            with source.open('w', encoding='utf-8', newline='') as f:
                w = csv.writer(f); w.writerow(('word', 'translation', 'tag')); w.writerow(('large', '长' * 140000, 'cet4'))
            a, b = root / 'a.wdb', root / 'b.wdb'
            report = builder.build(source, a); builder.build(source, b)
            self.assertEqual(a.read_bytes(), b.read_bytes()); self.assertGreater(report['max_entry_bytes'], 32768)
            ok, d, read = self.open(a.read_bytes()); self.assertTrue(ok)
            self.assertIn('长' * 140000, self.entry(d, 0)[1])

    def test_generic_phonetic_exam_examples_and_attribution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); source, catalog, row = self.inputs(root)
            body = row['content']['word']['content']
            body['phone'] = 'ˈæpəl'
            sentence = 'An apple example.'
            body['sentence'] = {'sentences': [{'sContent': sentence}]}
            body['realExamSentence'] = {'sentences': [
                {'sContent': sentence, 'sourceInfo': {'level': 'CET4', 'year': '2017.12'}},
                {'sContent': 'Another exam sentence.', 'sourceInfo': {'level': 'CET4'}}]}
            body['phrase'] = {'phrases': [{'pContent': 'apple tree', 'pCn': '苹果树'}]}
            body['speech'] = 'apple&type=2'
            path = root / 'kajweb.json'; self.write_rows(path, [row])
            output = root / 'dictionary.wdb'; report = builder.build(source, output, [path], catalog)
            text = self.contents(output)['apple']
            for value in ('ˈæpəl', sentence, 'Another exam sentence.', '苹果树', 'supplement'):
                self.assertIn(value, text)
            self.assertEqual(text.count(sentence), 1)
            attribution = next(a for a in report['example_attributions'] if 'year' in a['source_info'])
            self.assertEqual(attribution['sentence_sha256'], hashlib.sha256(sentence.encode()).hexdigest())
            self.assertEqual(attribution['record_id'], 'CET4_3_1')
            self.assertEqual(report['omitted_kajweb_fields'], {'kajweb:CET4_3:speech': 1})
            self.assertEqual(report['deduplicated_content'], 2)  # Example and shared CET4 tag.

    def test_rejected_rows_do_not_partially_merge_or_count_content(self):
        for damage in ('missing_title', 'surrogate', 'nul', 'bad_phrase'):
            with self.subTest(damage=damage), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); source, catalog, row = self.inputs(root)
                body = row['content']['word']['content']
                body['sentence'] = {'sentences': [{'sContent': 'must not leak'}] * 2}
                if damage == 'missing_title':
                    data = json.loads(catalog.read_text()); del data['data']['normalBooksInfo'][0]['title']
                    catalog.write_text(json.dumps(data), encoding='utf-8')
                else:
                    value = {'surrogate': '\ud800', 'nul': 'bad\0text', 'bad_phrase': 4}[damage]
                    body['phrase'] = {'phrases': [{'pContent': value}]}
                path = root / 'kajweb.json'; self.write_rows(path, [row])
                output = root / 'dictionary.wdb'; report = builder.build(source, output, [path], catalog)
                text = self.contents(output)['apple']
                self.assertNotIn('must not leak', text); self.assertNotIn('补充释义', text)
                self.assertEqual(report['deduplicated_content'], 0)
                self.assertEqual(len(report['rejected']), 1)
                self.assertEqual([s['id'] for s in report['sources']], ['ecdict'])

    def test_identical_book_in_json_and_zip_merges_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); source, catalog, row = self.inputs(root)
            path = root / 'a.json'; self.write_rows(path, [row, row])
            archive = root / 'b.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('CET4_3.JSON', json.dumps([row], indent=2))
            output = root / 'dictionary.wdb'
            report = builder.build(source, output, [path, archive, path], catalog)
            self.assertFalse(report['rejected']); self.assertFalse(report['rejected_sources'])
            self.assertEqual(len(report['duplicate_inputs']), 1)
            self.assertEqual(len(report['duplicate_records']), 2)
            self.assertEqual(report['records_read']['kajweb:CET4_3'], 3)
            self.assertEqual(report['records_accepted']['kajweb:CET4_3'], 1)
            self.assertEqual(len(report['sources'][1]['files']), 2)
            self.assertEqual(report['sources'][1]['output_entries'], 1)
            self.assertEqual(report['scopes'][0]['members'], 1)
            self.assertEqual(self.contents(output)['apple'].count('补充释义'), 1)

    def test_conflicting_book_versions_and_record_ids_exclude_whole_source(self):
        for damage in ('different_version', 'conflicting_id', 'malformed_version'):
            with self.subTest(damage=damage), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); source, catalog, first = self.inputs(root)
                second = json.loads(json.dumps(first))
                second['content']['word']['content']['trans'][0]['tranCn'] = 'other version'
                if damage == 'malformed_version':
                    second['content']['word']['content']['phrase'] = 4
                path = root / 'first.json'; other = root / 'second.json'
                self.write_rows(path, [first, second] if damage == 'conflicting_id' else [first])
                self.write_rows(other, [second])
                files = [path] if damage == 'conflicting_id' else [path, other]
                output = root / 'dictionary.wdb'; report = builder.build(source, output, files, catalog)
                text = self.contents(output)['apple']
                self.assertNotIn('补充释义', text); self.assertNotIn('other version', text)
                self.assertNotIn('kajweb:', text)
                self.assertEqual(report['rejected_sources'][0]['source'], 'kajweb:CET4_3')
                self.assertNotIn('kajweb:CET4_3', report['records_accepted'])

    def test_invalid_json_and_encoding_are_reported(self):
        cases = [b'[{invalid]', b'\xff', b'{"bookId":"CET4_3","bookId":"CET6_3"}', b'NaN']
        for content in cases:
            with self.subTest(content=content), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); source, catalog, _ = self.inputs(root)
                path = root / 'kajweb.json'; path.write_bytes(content)
                report = builder.build(source, root / 'dictionary.wdb', [path], catalog)
                self.assertEqual(len(report['rejected']), 1)
                self.assertEqual(report['input_files'][0]['sha256'], hashlib.sha256(content).hexdigest())

    def test_scope_mapping_and_catalog_conflicts_fail_before_output(self):
        for damage in ('unknown_scope', 'empty_name', 'conflicting_catalog'):
            with self.subTest(damage=damage), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); source, catalog, row = self.inputs(root)
                mapping = json.loads((ROOT / 'tools/scope_map.json').read_text(encoding='utf-8'))
                if damage == 'unknown_scope': mapping['ecdict_tags']['cet4'] = 'missing'
                elif damage == 'empty_name': mapping['scopes']['cet4'] = ''
                else:
                    data = json.loads(catalog.read_text())
                    data['data']['normalBooksInfo'].append(dict(data['data']['normalBooksInfo'][0], version='4'))
                    catalog.write_text(json.dumps(data), encoding='utf-8')
                map_path = root / 'map.json'; map_path.write_text(json.dumps(mapping), encoding='utf-8')
                path = root / 'kajweb.json'; self.write_rows(path, [row]); output = root / 'dictionary.wdb'
                with self.assertRaises(ValueError): builder.build(source, output, [path], catalog, mapping=map_path)
                self.assertFalse(output.exists())

    def test_source_counts_follow_selected_scope_and_ecdict_line_endings(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); source, catalog, row = self.inputs(root)
            source.write_text('word,translation,tag\napple,line1\\r\\nline2,cet4\nother,another,cet6\n', encoding='utf-8')
            path = root / 'kajweb.json'; self.write_rows(path, [row]); output = root / 'dictionary.wdb'
            report = builder.build(source, output, [path], catalog, include_scopes=['cet6'])
            self.assertEqual(report['entries'], 1)
            self.assertEqual([s['output_entries'] for s in report['sources']], [1, 0])
            self.assertEqual(report['example_attributions'], [])
            self.assertEqual(report['cross_source_entries'], 0)
            all_output = root / 'all.wdb'; builder.build(source, all_output, [path], catalog)
            text = self.contents(all_output)['apple']
            self.assertIn('line1', text); self.assertIn('line2', text); self.assertNotIn('\\r', text)

    def test_actual_source_samples(self):
        source_root = ROOT / 'output/source'
        archives = sorted((source_root / 'kajweb').glob('*.zip'))
        catalog = source_root / 'kajweb/bookLists.txt'
        if not archives or not catalog.exists(): self.skipTest('No downloaded kajweb source samples')
        selected, words = [], set()
        for archive in archives:
            rows = [row for _, _, row in builder.input_records(archive)]
            samples = [rows[0]]
            for field in ('phone', 'realExamSentence'):
                candidate = next((r for r in rows if r['content']['word']['content'].get(field)), None)
                if candidate and candidate not in samples: samples.append(candidate)
            selected.append((archive.stem, samples)); words.update(r['headWord'] for r in samples)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); source = root / 'sample.csv'; files = []
            with (source_root / 'ecdict.csv').open(encoding='utf-8-sig', newline='') as f, source.open('w', encoding='utf-8', newline='') as out:
                reader = csv.DictReader(f); writer = csv.DictWriter(out, reader.fieldnames); writer.writeheader()
                writer.writerows(row for row in reader if row['word'] in words)
            for name, rows in selected:
                path = root / (name + '.json'); self.write_rows(path, rows); files.append(path)
            output = root / 'sample.wdb'; report = builder.build(source, output, files, catalog)
            self.assertFalse(report['rejected']); self.assertFalse(report['rejected_sources'])
            content = self.contents(output)
            for name, rows in selected:
                for row in rows:
                    word, body = row['headWord'], row['content']['word']['content']
                    if body.get('phone'): self.assertIn(builder.normalize(body['phone']), content[word])
                    for example in body.get('realExamSentence', {}).get('sentences', []):
                        self.assertIn(builder.normalize(example['sContent']), content[word])
            self.assertEqual(len(report['sources']), len(archives) + 1)
            self.assertTrue(report['example_attributions'])

    def test_real_dictionary(self):
        if not args.dictionary: self.skipTest('No real dictionary argument')
        blob = args.dictionary.read_bytes(); ok, d, read = self.open(blob); self.assertTrue(ok)
        for word in ('cancel', 'explosive'):
            index, found = C.c_uint32(), C.c_bool()
            self.assertTrue(lib.words_dictionary_find(C.byref(d), word.encode(), C.byref(index), C.byref(found)))
            self.assertTrue(found.value); self.assertEqual(self.entry(d, index.value)[0], word)
        for i in range(d.count):
            self.entry(d, i)
        print(f'Validated every record in real WDB2: {d.count} entries')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--library', type=Path, required=True); parser.add_argument('--dictionary', type=Path)
    args = parser.parse_args(); lib = C.CDLL(str(args.library.resolve()))
    for name, types in {
        'open': [C.POINTER(Dictionary), READ, C.c_void_p, C.c_uint32],
        'entry_size': [C.POINTER(Dictionary), C.c_uint32, C.POINTER(C.c_uint32)],
        'read_entry': [C.POINTER(Dictionary), C.c_uint32, C.c_void_p, C.c_size_t, C.POINTER(Entry)],
        'scope': [C.POINTER(Dictionary), C.c_uint32, C.POINTER(Scope)],
        'member': [C.POINTER(Dictionary), C.POINTER(Scope), C.c_uint32, C.POINTER(C.c_uint32)],
        'find': [C.POINTER(Dictionary), C.c_char_p, C.POINTER(C.c_uint32), C.POINTER(C.c_bool)],
        'unchanged': [C.POINTER(Dictionary)]}.items():
        fn = getattr(lib, 'words_dictionary_' + name); fn.argtypes = types; fn.restype = C.c_bool
    lib.words_entry_parse.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(Entry)]; lib.words_entry_parse.restype = C.c_bool
    lib.words_entry_text.argtypes = [C.POINTER(Entry), C.c_uint, C.c_void_p, C.c_size_t]; lib.words_entry_text.restype = C.c_size_t
    unittest.main(argv=[sys.argv[0]], verbosity=2)
