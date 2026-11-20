#!/usr/bin/env python3
"""Build the portable ChatGPT skill archive from the repository sources."""

import argparse
from pathlib import Path
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile


SKILL_NAME = "ai-board-embedded-debug"
TOOL_FILES = ("board_rpc.py", "lan_smoke.py", "serial_capture.py")
FIRMWARE_DIR = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = FIRMWARE_DIR / "build" / f"{SKILL_NAME}.zip"


def package_skill(output: Path) -> None:
    skill_dir = FIRMWARE_DIR / "skills" / SKILL_NAME
    sources = [skill_dir / "SKILL.md", *sorted((skill_dir / "references").rglob("*.md"))]
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=output.parent) as staging_dir:
        staged = Path(staging_dir) / output.name
        with ZipFile(staged, "w", ZIP_DEFLATED, compresslevel=9) as archive:
            for source in sources:
                archive.write(source, f"{SKILL_NAME}/{source.relative_to(skill_dir).as_posix()}")
            for filename in TOOL_FILES:
                archive.write(FIRMWARE_DIR / "tools" / filename, f"{SKILL_NAME}/scripts/{filename}")
        staged.replace(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Path for the generated skill ZIP")
    options = parser.parse_args()
    output = options.output.resolve()
    if output.suffix.lower() != ".zip":
        parser.error("--output must end in .zip")
    package_skill(output)
    print(output)


if __name__ == "__main__":
    main()
