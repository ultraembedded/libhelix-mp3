"""Keep runtime floating-point types/operations out of application C sources."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
paths = sorted(root.glob("*.c")) + sorted(root.glob("*.inc")) + sorted(root.glob("*.h"))
paths += sorted((root / "real").glob("*.[ch]")) + sorted((root / "pub").glob("*.h"))
# Consume strings before comment markers, so URLs/--double and explanatory
# comments cannot be mistaken for executable code.
ignored = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*', re.S)
floating = re.compile(r"\b(?:float|double|atof|strtod|sqrt|sqrtf|pow|powf)\b|(?<![\w.])(?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?[fFlL]?|\b\d+[eE][+-]?\d+[fFlL]?\b")
failures = []
for path in paths:
    code = ignored.sub(lambda match: "\n" * match.group().count("\n"), path.read_text())
    for match in floating.finditer(code):
        failures.append(f"{path.relative_to(root)}:{code.count(chr(10), 0, match.start()) + 1}: {match.group()}")
assert not failures, "Runtime floating-point code:\n" + "\n".join(failures)
print(f"Integer application source check: {len(paths)} files passed")
