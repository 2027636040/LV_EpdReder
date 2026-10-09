"""Run against a native build of the unchanged C core and the pinned Py-FSRS checkout."""
import argparse
import csv
import ctypes as C
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import math
from pathlib import Path
import random
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('builder', ROOT / 'tools/build_dictionary.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class Card(C.Structure):
    _fields_ = [('stability', C.c_double), ('difficulty', C.c_double),
                ('last_review', C.c_int64), ('due', C.c_int64),
                ('reviews', C.c_uint32), ('lapses', C.c_uint32),
                ('phase', C.c_uint8), ('step', C.c_uint8)]


READ = C.CFUNCTYPE(C.c_bool, C.c_void_p, C.c_uint32, C.c_void_p, C.c_size_t)


class Dictionary(C.Structure):
    _fields_ = [('read', READ), ('context', C.c_void_p)] + [
        (name, C.c_uint32) for name in ('count', 'aliases', 'index', 'alias_index', 'data', 'size')]


class Matches(C.Structure):
    _fields_ = [('count', C.c_uint32), ('entry', C.c_uint32 * 8), ('more', C.c_bool)]


class CoreTests(unittest.TestCase):
    def test_real_dictionary(self):
        if not args.dictionary or not args.csv:
            self.skipTest('real dictionary/CSV not supplied')
        with args.dictionary.open('rb') as stream:
            @READ
            def read(context, offset, buffer, size):
                stream.seek(offset)
                data = stream.read(size)
                if len(data) != size:
                    return False
                C.memmove(buffer, data, size)
                return True
            dictionary = Dictionary()
            self.assertTrue(lib.words_dictionary_open(C.byref(dictionary), read, None, args.dictionary.stat().st_size))
            rng, sampled = random.Random(928), {}
            for index in rng.sample(range(dictionary.count), min(1000, dictionary.count)):
                buffer, fields = C.create_string_buffer(32768), (C.c_char_p * 6)()
                self.assertTrue(lib.words_dictionary_entry(C.byref(dictionary), index, buffer, len(buffer), fields))
                values = tuple(field.decode() for field in fields)
                sampled[values[0]] = values
                self.assertIn(values[0], self.lookup(dictionary, values[0])[0])
            with args.csv.open(encoding='utf-8-sig', newline='') as source:
                for row in csv.DictReader(source):
                    word = builder.normalize(row.get('word'))
                    if word in sampled:
                        self.assertEqual(sampled.pop(word), tuple(builder.normalize(row.get(name)) for name in builder.FIELDS))
            self.assertFalse(sampled)
        print('Dictionary: 1000 random entries matched source CSV and exact lookup')

    def test_fsrs_reference_sequences(self):
        scheduler = Scheduler(enable_fuzzing=False)
        rng = random.Random(2892026)
        compared = 0
        for sequence in range(128):
            actual, reference = Card(), ReferenceCard()
            now = datetime(2026, 9, 28, tzinfo=timezone.utc)
            lapses = 0
            for review in range(128):
                # All initial ratings, short-term repeats, late reviews, and long gaps.
                rating = sequence % 4 + 1 if not review else rng.randint(1, 4)
                if review:
                    now += timedelta(seconds=rng.choice([0, 60, 330, 600, 86399, 86400, 86401, 86400 * 21]))
                if actual.phase == 2 and rating == 1:
                    lapses += 1
                updated = Card()
                self.assertTrue(lib.words_fsrs_review(C.byref(actual), rating, int(now.timestamp()), C.byref(updated)))
                reference, _ = scheduler.review_card(reference, Rating(rating), now)
                self.assertEqual(updated.phase, int(reference.state))
                self.assertEqual(updated.step, reference.step or 0)
                self.assertEqual(updated.due, int(reference.due.timestamp()))
                self.assertEqual(updated.last_review, int(reference.last_review.timestamp()))
                self.assertTrue(math.isclose(updated.stability, reference.stability, rel_tol=1e-11))
                self.assertTrue(math.isclose(updated.difficulty, reference.difficulty, rel_tol=1e-11))
                self.assertEqual(updated.reviews, review + 1)
                self.assertEqual(updated.lapses, lapses)
                actual = updated
                compared += 1
        print(f'FSRS: {compared} transitions matched pinned Py-FSRS')

    def test_invalid_review_does_not_modify_output(self):
        before = Card(2, 5, 100, 200, 3, 0, 2, 0)
        result = Card(999, 999, 999, 999, 999, 999, 9, 9)
        unchanged = bytes(result)
        for rating, timestamp in [(0, 100), (5, 100), (3, 99), (3, -1), (3, 2**63 - 1)]:
            self.assertFalse(lib.words_fsrs_review(C.byref(before), rating, timestamp, C.byref(result)))
            self.assertEqual(bytes(result), unchanged)
        before.stability = float('nan')
        self.assertFalse(lib.words_fsrs_review(C.byref(before), 3, 100, C.byref(result)))

    def create_dictionary(self, directory):
        source = directory / 'input.csv'
        rows = [
            ['apple', 'ˈæpl', 'n. 苹果\n苹果树', 'A fruit, often red.', 's:apples', 'cet4'],
            ['Apple', '', '苹果公司', '', '', ''],
            ['go', '', 'v. 去', '', 'p:went/d:gone/i:going/3:goes', 'cet4'],
            ['good', '', 'a. 好的', '', 'r:better/t:best', 'cet4'],
            ['well', '', 'adv. 好', '', 'r:better/t:best', 'cet4'],
            ['bad', '', '', '', '', 'cet4'],
            ['apple', '', '重复词条', '', '', 'cet4'],
            ['x' * 96, '', '过长', '', '', ''],
            ['café', '', '咖啡馆', '', '', ''],
        ] + [[f'prefix{i}', '', '释义', '', '', ''] for i in range(10)]
        with source.open('w', newline='', encoding='utf-8') as stream:
            writer = csv.writer(stream)
            writer.writerow(builder.FIELDS)
            writer.writerows(rows)
        destination = directory / 'library.wdb'
        report = builder.build(source, destination)
        return source, destination.read_bytes(), report

    def open_blob(self, blob):
        calls = []
        @READ
        def read(context, offset, buffer, size):
            calls.append((offset, size))
            if offset + size > len(blob):
                return False
            C.memmove(buffer, bytes(blob[offset:offset + size]), size)
            return True
        dictionary = Dictionary()
        ok = lib.words_dictionary_open(C.byref(dictionary), read, None, len(blob))
        return ok, dictionary, read, calls

    def lookup(self, dictionary, text):
        matches = Matches()
        self.assertTrue(lib.words_dictionary_search(C.byref(dictionary), text.encode(), C.byref(matches)))
        words = []
        for i in range(matches.count):
            buffer = C.create_string_buffer(32768)
            fields = (C.c_char_p * 6)()
            self.assertTrue(lib.words_dictionary_entry(C.byref(dictionary), matches.entry[i], buffer, len(buffer), fields))
            words.append(fields[0].decode())
        return words, matches.more

    def test_dictionary_search_and_deterministic_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source, blob, report = self.create_dictionary(directory)
            builder.build(source, directory / 'again.wdb')
            self.assertEqual(blob, (directory / 'again.wdb').read_bytes())
            self.assertEqual(len(report['duplicates']), 1)
            self.assertEqual(len(report['rejected']), 1)
            self.assertEqual(hashlib.sha256(blob[64:]).digest()[:24], blob[40:64])
            ok, dictionary, callback, calls = self.open_blob(blob)
            self.assertTrue(ok)
            self.assertEqual(self.lookup(dictionary, 'APPLE')[0], ['Apple', 'apple'])
            self.assertEqual(self.lookup(dictionary, 'apples')[0], ['apple'])
            self.assertEqual(self.lookup(dictionary, 'better')[0], ['good', 'well'])
            self.assertEqual(self.lookup(dictionary, 'went')[0], ['go'])
            self.assertEqual(self.lookup(dictionary, 'café')[0], ['café'])
            self.assertEqual(self.lookup(dictionary, 'zzzz')[0], [])
            self.assertEqual(len(self.lookup(dictionary, 'prefix')[0]), 8)
            self.assertTrue(self.lookup(dictionary, 'prefix')[1])
            self.assertLess(max(size for _, size in calls), 1024)
            book = builder.build(source, directory / 'cet4.wdb', 'cet4')
            self.assertEqual(book['entries'], 4)
            self.assertEqual(len(book['rejected']), 1)

    def test_corrupt_headers_and_records(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, blob, _ = self.create_dictionary(Path(temporary))
            for offset in (0, 8, 12, 16, 20, 24, 28, 32, 36):
                corrupt = bytearray(blob)
                corrupt[offset:offset + 4] = b'\xff' * 4
                self.assertFalse(self.open_blob(corrupt)[0], offset)
            corrupt = bytearray(blob)
            struct.pack_into('<I', corrupt, 64 + 96, len(blob) + 1)
            ok, dictionary, callback, _ = self.open_blob(corrupt)
            self.assertTrue(ok)
            buffer, fields = C.create_string_buffer(32768), (C.c_char_p * 6)()
            self.assertFalse(lib.words_dictionary_entry(C.byref(dictionary), 0, buffer, len(buffer), fields))
            self.assertFalse(lib.words_dictionary_entry(C.byref(dictionary), dictionary.count, buffer, len(buffer), fields))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--dictionary', type=Path)
    parser.add_argument('--csv', type=Path)
    args = parser.parse_args()
    reference_hash = hashlib.sha256((args.reference / 'fsrs/scheduler.py').read_text(encoding='utf-8').encode()).hexdigest()
    if reference_hash != 'dd04d86ba98ee22106f6bbdb7eede00fa3c848dbf15d18e33db4584f333bac46':
        parser.error(f'Reference scheduler must match commit {builder.FSRS_REVISION}')
    sys.path.insert(0, str(args.reference))
    from fsrs import Scheduler, Card as ReferenceCard, Rating
    lib = C.CDLL(str(args.library.resolve()))
    lib.words_fsrs_review.argtypes = [C.POINTER(Card), C.c_int, C.c_int64, C.POINTER(Card)]
    lib.words_fsrs_review.restype = C.c_bool
    lib.words_dictionary_open.argtypes = [C.POINTER(Dictionary), READ, C.c_void_p, C.c_uint32]
    lib.words_dictionary_open.restype = C.c_bool
    lib.words_dictionary_search.argtypes = [C.POINTER(Dictionary), C.c_char_p, C.POINTER(Matches)]
    lib.words_dictionary_search.restype = C.c_bool
    lib.words_dictionary_entry.argtypes = [C.POINTER(Dictionary), C.c_uint32, C.c_void_p, C.c_size_t, C.POINTER(C.c_char_p)]
    lib.words_dictionary_entry.restype = C.c_bool
    unittest.main(argv=[sys.argv[0]], verbosity=2)
