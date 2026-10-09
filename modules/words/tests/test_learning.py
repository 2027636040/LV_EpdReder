"""Exercise the production learning worker/store with host-only filesystem and RTOS shims."""
import argparse
import csv
import ctypes as C
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('builder', ROOT / 'tools/build_dictionary.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library', type=Path, required=True)
args = parser.parse_args()
lib = C.CDLL(str(args.library.resolve()))
lib.words_test_file.argtypes = [C.c_char_p, C.c_void_p, C.c_uint32]
lib.words_test_file.restype = C.c_bool
lib.words_test_learning.restype = C.c_int
with tempfile.TemporaryDirectory(prefix='words-learning-') as folder:
    directory = Path(folder)
    source = directory / 'words.csv'
    with source.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(builder.FIELDS)
        for name in ['Apple', 'apple'] + [f'word{i:03}' for i in range(80)]:
            writer.writerow([name, '', 'n. 测试释义', 'A test entry.', '', 'cet4'])
    path = directory / 'book.wdb'
    builder.build(source, path)
    data = path.read_bytes()
    for filename in (b'/flash/apps/words/res/library.wdb', b'/sdcard/words/books/second.wdb'):
        assert lib.words_test_file(filename, data, len(data))
    result = lib.words_test_learning()
    if result:
        raise SystemExit(f'Learning integration failed at C harness line {result}')
print('Learning integration passed: resume, ratings, deduplication, quotas, day rollover, collection, pause, import, save retry, 9217-record reload, allocation failure/retry, identity and resource release.')
