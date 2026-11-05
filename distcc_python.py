"""Keep compiler-helper dependencies available in Python build environments."""
import site
import sys

# Python builds can disable global site-packages (for example, Gentoo's
# GENTOO_CPYTHON_BUILD). Restore this tool's installed package directories
# without changing the environment inherited by compilers and build tools.
for directory in site.getsitepackages():
    if directory not in sys.path:
        site.addsitedir(directory)
