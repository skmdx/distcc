from typing import Sequence, TypedDict

version: str

class ScanResult(TypedDict):
    files: dict[str, bytes]
    lookups: dict[str, tuple[int, int, int] | tuple[int, int, int, int, int, int] | None]
    dependencies: list[str]

def scan(argv: Sequence[str], cwd: str) -> ScanResult: ...
def include_probes(source: bytes) -> list[tuple[int, str]]: ...
def conditional_expressions(source: bytes) -> list[str]: ...
