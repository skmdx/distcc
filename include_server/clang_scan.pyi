from typing import Sequence, TypedDict

version: str
gcc_scan_dir: str

class ScanResult(TypedDict):
    files: dict[str, bytes]
    lookups: dict[str, tuple[int, int, int] | tuple[int, int, int, int, int, int] | None]
    dependencies: list[str]

def scan(argv: Sequence[str], cwd: str) -> ScanResult: ...
def compile(argv: Sequence[str], original: str, root: str, cwd: str, pump: bool) -> int: ...
