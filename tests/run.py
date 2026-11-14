import pathlib
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix='kitty-litter-tests-') as root:
    subprocess.run([str(pathlib.Path(sys.argv[1]).resolve()),root],check=True)
