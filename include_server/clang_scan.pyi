from typing import Sequence, TypedDict

version: str
gcc_scan_dir: str

class ScanResult(TypedDict):
    files: dict[str, memoryview]
    lookups: dict[str, tuple[int, int, int] | tuple[int, int, int, int, int, int] | None]
    dependencies: list[str]
    preamble_hit: bool
    preamble_built: bool
    preamble_bytes: int

def scan(argv: Sequence[str], cwd: str, use_cache: bool = True) -> ScanResult: ...
def tokenize_command(command: str) -> list[str]: ...
def compile(argv: Sequence[str], original: str, root: str, cwd: str, pump: bool) -> int: ...
