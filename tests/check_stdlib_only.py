#!/usr/bin/env python3
"""Fail if any Python in this repo imports something outside the standard library.

The web control panel has no third-party dependencies, which is why it needs no venv, no
pip and no build step -- it just runs. That is a property worth enforcing rather than
asserting, because it is easy to erode one convenient import at a time.

If this check fails, the question is not "how do we install that package". It is whether the
dependency is worth what it costs here: Debian 12+ marks system Python as externally
managed, so pip into it is blocked, leaving a venv (a setup step and a failure mode when the
interpreter moves under it) or an apt package (whatever the distro happens to ship). Often
the honest answer is that the standard library was fine.
"""
from __future__ import annotations

import ast
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKIP_DIRS = {"build", "build-fb", ".git", "__pycache__"}


def imported_modules(path: pathlib.Path) -> set[str]:
    tree = ast.parse(path.read_text(), filename=str(path))
    found: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            found.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            # level > 0 is a relative import, which is by definition local to this repo.
            if node.level == 0 and node.module:
                found.add(node.module.split(".")[0])
    return found


def main() -> int:
    stdlib = set(sys.stdlib_module_names)
    local = {p.stem for p in ROOT.rglob("*.py")}
    failures: list[tuple[pathlib.Path, str]] = []
    checked = 0

    for path in sorted(ROOT.rglob("*.py")):
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        checked += 1
        for module in sorted(imported_modules(path)):
            if module in stdlib or module in local:
                continue
            failures.append((path.relative_to(ROOT), module))

    if failures:
        print(f"  FAIL  {len(failures)} non-stdlib import(s) across {checked} file(s):")
        for path, module in failures:
            print(f"        {path}: {module}")
        print("        See the docstring in tests/check_stdlib_only.py before adding a venv.")
        return 1

    print(f"  PASS  {checked} Python files import only the standard library (no venv needed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
