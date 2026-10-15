"""Check the maintained documentation entry points and application designs."""
from pathlib import Path
import re
import sys
import unittest
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from native_apps import PACKAGE_BUILDERS

PAGES = ('README.md', 'src/services/README.md', 'docs/README.md', 'docs/ARCHITECTURE.md',
         'docs/DESIGN_INTENT.md', 'docs/PLATFORM_ROADMAP.md', 'docs/TECHNICAL_RESERVES.md',
         'docs/APP_SDK.md', 'docs/RECOVERY.md', 'docs/CONTENT_HANDLERS.md',
         'docs/BOARD_TEMPLATES.md', 'docs/RELEASING.md', 'docs/BUILDING.md',
         'docs/APPLICATION_DEVELOPMENT.md', 'docs/APPLICATION_LIFECYCLE.md',
         'docs/PLATFORM_VALIDATION.md', 'docs/PLATFORM_IMPLEMENTATION_TODO.md')


class DocumentationTests(unittest.TestCase):
    def test_application_designs(self):
        for app in (*PACKAGE_BUILDERS, 'hello'):
            with self.subTest(app=app):
                self.assertTrue((ROOT / 'modules' / app / 'DESIGN.md').is_file())

    def test_local_links(self):
        pages = [ROOT / name for name in PAGES]
        pages += list((ROOT / 'docs/reserves').glob('*.md'))
        pages += [ROOT / 'modules' / app / 'DESIGN.md' for app in (*PACKAGE_BUILDERS, 'hello')]
        for page in pages:
            with self.subTest(page=page.relative_to(ROOT)):
                source = page.read_text(encoding='utf-8')
                for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)', source):
                    target = target.strip('<>')
                    if '://' in target or target.startswith('#'):
                        continue
                    target = unquote(target.split('#', 1)[0])
                    self.assertTrue((page.parent / target).exists(), f'{page}: {target}')


if __name__ == '__main__':
    unittest.main()
