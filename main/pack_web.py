from pathlib import Path
import gzip
import sys

source, output = map(Path, sys.argv[1:])
output.write_bytes(gzip.compress(source.read_bytes(), compresslevel=9, mtime=0))
