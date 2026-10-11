"""Host tests for LCD DevKit expander adaptation and connector remapping."""
from pathlib import Path
import json
import re
import shutil
import subprocess
import tempfile


def check_board(repo):
    board = repo / "boards/sf32lb57-lcd-gyd7n6_epd"
    old = (repo / "SiFli-SDK/customer/boards/sf32lb57-dpi-hdk_base/bsp_lcd_tp.c").read_text(encoding="utf-8")
    new = (board / "bsp_lcd_tp.c").read_text(encoding="utf-8")
    old_dpi = dict((int(pin), signal) for pin, signal in re.findall(
        r"HAL_PIN_Set\(PAD_PA(\d+), LCDC1_DPI_(\w+),", old))
    # LCD DevKit FPC1 pad assignment, schematic LCD sheet and IO table column O.
    new_dpi = dict(zip(
        [f"R{i}" for i in range(8)] + [f"G{i}" for i in range(8)] +
        [f"B{i}" for i in range(8)] + ["CLK", "HSYNC", "VSYNC", "DE"],
        [24,25,26,27,28,29,30,31,32,33,37,38,39,40,41,42,
         43,44,45,52,53,54,46,47,48,49,51,50]))
    def tcon(source):
        return {signal: int(pin) for pin, signal in re.findall(
            r"HAL_PIN_Set\(PAD_PA(\d+), LCDC1_TCON_(\w+),", source)}
    expected = {signal: new_dpi[old_dpi[pin]] for signal, pin in tcon(old).items()}
    assert tcon(new) == expected
    assert len(set(expected.values())) == len(expected) == 22
    project = repo / "project"
    old_project = project / "dpi-hdk_lb57gyd7n6_epd_hcpu"
    new_project = project / "sf32lb57-lcd-gyd7n6_epd_hcpu"
    assert json.loads((old_project / "ptab.json").read_text()) == json.loads(
        (new_project / "ptab.json").read_text())
    assert 'default 51' in (board / 'Kconfig.board').read_text()
    assert 'CONFIG_SDMMC1_BUS_WIDTH_1_ONLY=y' in (new_project / 'proj.conf').read_text()
    print("Connector mapping (22 TCON signals), partition layout and SD width: PASS")


def main():
    tests = Path(__file__).resolve().parent
    repo = tests.parents[1]
    check_board(repo)
    compiler = shutil.which("cl")
    if not compiler:
        raise SystemExit("Activate an MSVC developer environment for the expander tests.")
    source = (repo / "boards/sf32lb57-lcd-gyd7n6_epd/board_ioex.c").read_text(encoding="utf-8")
    # Replace only target includes. Exercise the production implementation itself.
    source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.MULTILINE)
    with tempfile.TemporaryDirectory(prefix="lcd-board-") as directory:
        output = Path(directory)
        (output / "ioex_under_test.h").write_text(source, encoding="utf-8")
        target = output / "ioex.exe"
        subprocess.run([compiler, "/nologo", "/std:c11", "/W4", "/WX", "/TC",
                        "/D_CRT_SECURE_NO_WARNINGS", f"/I{output}",
                        str(tests / "test_lcd_board_ioex.c"),
                        f"/Fo{output / 'ioex.obj'}", f"/Fe{target}"], check=True)
        for case in ("normal", "id", "read", "write", "alloc", "bus"):
            subprocess.run([str(target), case], check=True)


if __name__ == "__main__":
    main()
