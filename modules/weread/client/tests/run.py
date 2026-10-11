"""Build and run protocol tests with a POSIX host C compiler."""
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
client = here.parent
sources = [here / "test_client.c"] + [client / name for name in (
    "wr_client.c", "wr_json.c", "wr_models.c", "wr_protocol.c",
    "private/wr_cjson.c", "private/wr_md5.c", "private/wr_base64.c")]
with tempfile.TemporaryDirectory(prefix="weread-client-") as temporary:
    executable = Path(temporary) / "client_test"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1",
               "-DWR_MBEDTLS_SELF_TEST", "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer", "-fno-sanitize-recover=all",
               "-I" + str(here / "include"), "-I" + str(client),
               "-I" + str(client.parents[1] / "common/http")]
    subprocess.run(command + [str(path) for path in sources] + ["-lm", "-o", str(executable)], check=True)
    subprocess.run([str(executable)], cwd=temporary, check=True)
