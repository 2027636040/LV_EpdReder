"""Exercise content routing with the actual SDK intent serializer and production C."""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import tempfile


def without_includes(text):
    return re.sub(r'^#include[^\n]*\n', '', text, flags=re.MULTILINE)


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    root = tests.parents[1]
    sdk = root / 'SiFli-SDK'
    compiler = shutil.which('cl')
    if not compiler:
        raise SystemExit('Run in an MSVC developer environment (VsDevCmd.bat -arch=x64).')
    with tempfile.TemporaryDirectory(prefix='app-content-') as directory:
        out = Path(directory)
        (out / 'rtthread.h').write_text(
            '#include <stdlib.h>\n#include <string.h>\n#define rt_malloc malloc\n'
            '#define rt_free free\n#define rt_realloc realloc\n#define rt_memcpy memcpy\n'
            '#define rt_memset memset\n', encoding='utf-8')
        def source(path):
            return (root / path).read_text(encoding='utf-8')
        constants = source('SiFli-SDK/rtos/rtthread/include/rtdef.h')
        constants += source('SiFli-SDK/middleware/include/gui_app_fwk.h')
        constants += source('src/services/storage.h')
        (out / 'constants.h').write_text('\n'.join(
            line for line in constants.splitlines()
            if re.match(r'#define (RT_E[A-Z]+|RT_ERROR|GUI_APP_ID_MAX_LEN|STORAGE_\w+)\s', line)
        ), encoding='utf-8')
        headers = ''.join(without_includes(source(path)) for path in (
            'src/ui/platform/app_catalog.h', 'src/ui/platform/app_content.h', 'src/ui/platform/app_package.h'))
        (out / 'content_headers.h').write_text(headers, encoding='utf-8')
        package = source('src/ui/platform/app_package.c')
        selected = ''.join(function(package, signature) for signature in (
            'static bool path_join(', 'static bool relative_path(', 'static const char *string_value(',
            'static bool positive_integer(', 'static bool content_extensions_valid(',
            'bool app_package_handles_path(', 'bool app_package_metadata('))
        (out / 'package_under_test.h').write_text(selected, encoding='utf-8')
        (out / 'content_under_test.h').write_text(
            without_includes(source('src/ui/platform/app_content.c')), encoding='utf-8')
        sdk_intent = '#include "test_app_content_sdk_stubs.h"\n'
        sdk_intent += '#include "intent_int.h"\n'
        sdk_intent += without_includes(source('SiFli-SDK/middleware/app_fwk/intent.c'))
        (out / 'sdk_intent.c').write_text(sdk_intent, encoding='utf-8')
        (out / 'sdk_run_under_test.h').write_text(function(
            source('SiFli-SDK/middleware/app_fwk/gui_app_fwk.c'), 'int gui_app_run_by_intent('), encoding='utf-8')
        common = [compiler, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/Od',
                  '/D_CRT_SECURE_NO_WARNINGS', f'/I{out}', f'/I{tests}',
                  f'/I{sdk / "middleware/app_fwk"}', f'/I{sdk / "middleware/include"}',
                  f'/I{sdk / "external/cJSON-1.7.17"}']
        if args.asan:
            common += ['/fsanitize=address', '/Zi']
        # The SDK's existing intent implementation has unused locals and size_t
        # conversions; suppress those only for its own translation unit.
        subprocess.run(common + ['/wd4101', '/wd4189', '/wd4244', '/wd4267', '/c',
                                  str(out / 'sdk_intent.c')], cwd=out, check=True)
        subprocess.run(common + ['/c', str(sdk / 'external/cJSON-1.7.17/cJSON.c')], cwd=out, check=True)
        target = out / 'content.exe'
        subprocess.run(common + [str(tests / 'test_app_content.c'), 'sdk_intent.obj', 'cJSON.obj',
                                  f'/Fe{target}'], cwd=out, check=True)
        subprocess.run([str(target)], cwd=out, check=True)
        books = source('modules/books/app_main.c')
        types = source('modules/books/books_ui.h')
        (out / 'books_page_types.h').write_text(
            types[types.index('#define WIDTH'):types.index('extern books_page_t *current;')], encoding='utf-8')
        lifecycle = books[books.index('books_page_t *current;'):books.index('static bool page_input_ready(')]
        lifecycle += ''.join(function(books, signature) for signature in (
            'static bool page_open(', 'static void content_process(',
            'static void page_message(gui_app_msg_type_t message, void *parameter)\n{', 'int app_main('))
        (out / 'books_lifecycle_under_test.h').write_text(lifecycle, encoding='utf-8')
        content = source('src/ui/platform/app_content.c')
        (out / 'receive_under_test.h').write_text(''.join(function(content, signature) for signature in (
            'static bool valid_path(', 'static rt_err_t file_ready(', 'rt_err_t app_content_receive(')), encoding='utf-8')
        schedule = source('SiFli-SDK/middleware/app_fwk/app_schedule.c')
        block = function(schedule, '        case GUI_APP_MSG_RUN_APP_I:\n        case GUI_APP_MSG_RUN_APP:')
        block = block[block.index('{'):]
        (out / 'sdk_dispatch_under_test.h').write_text(
            'static void dispatch_content(gui_app_msg_t *p_msg, mailbox_t *msg_mbx)\n' + block, encoding='utf-8')
        target = out / 'lifecycle.exe'
        subprocess.run(common + ['/wd4100', '/wd4189', '/wd4505',
                                  str(tests / 'test_content_lifecycle.c'), 'sdk_intent.obj',
                                  f'/Fe{target}'], cwd=out, check=True)
        subprocess.run([str(target)], cwd=out, check=True)
        gallery = source('modules/gallery/gallery_ui.c')
        gallery_types = source('modules/gallery/gallery.h')
        (out / 'gallery_types.h').write_text(
            gallery_types[gallery_types.index('#define GALLERY_ROWS'):gallery_types.index('typedef raster_decoder_t')],
            encoding='utf-8')
        gallery_ui = gallery[gallery.index('static char launch_path'):gallery.index('static void browser_scan(')]
        gallery_ui += ''.join(function(gallery, signature) for signature in (
            'static bool browser_open_content(', 'static void browser_message(',
            'static void release_image(', 'static void viewer_message(gui_app_msg_type_t message, void *parameter)\n{',
            'int app_main('))
        (out / 'gallery_lifecycle_under_test.h').write_text(gallery_ui, encoding='utf-8')
        target = out / 'gallery_lifecycle.exe'
        subprocess.run(common + ['/wd4100', '/wd4189', '/wd4505',
                                  str(tests / 'test_content_gallery.c'), 'sdk_intent.obj',
                                  f'/Fe{target}'], cwd=out, check=True)
        subprocess.run([str(target)], cwd=out, check=True)
        (out / 'books_data_types.h').write_text(''.join(without_includes(source(path)) for path in (
            'modules/books/bookshelf.h', 'modules/books/ui_bookshelf_data.h')), encoding='utf-8')
        (out / 'books_data_under_test.h').write_text(
            without_includes(source('modules/books/ui_bookshelf_data.c')), encoding='utf-8')
        target = out / 'books_data.exe'
        subprocess.run(common + ['/wd4018', '/wd4267', '/wd4244', str(tests / 'test_content_books_data.c'),
                                  f'/Fe{target}'], cwd=out, check=True)
        subprocess.run([str(target)], cwd=out, check=True)


if __name__ == '__main__':
    main()
