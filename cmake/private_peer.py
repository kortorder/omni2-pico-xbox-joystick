"""Generate an owner-only compiler input for one private build; print no peer data."""
import os
import stat
import sys
from pathlib import Path


def outside(path, parent):
    try:
        path.relative_to(parent)
    except ValueError:
        return True
    return False


def generate(peer_file, destination, repository):
    source = Path(peer_file)
    dest = Path(destination)
    repo = Path(repository).resolve()
    if not source.is_absolute() or not dest.is_absolute():
        raise ValueError("Use absolute paths for private build inputs and outputs.")
    if not outside(source.resolve(), repo) or not outside(dest.resolve(), repo):
        raise ValueError("Keep private pin inputs and the private build directory outside the repository.")
    flags = os.O_RDONLY | os.O_NOFOLLOW
    with os.fdopen(os.open(source, flags), "rb") as f:
        mode = os.fstat(f.fileno())
        if not stat.S_ISREG(mode.st_mode) or mode.st_uid != os.getuid() or mode.st_mode & 0o077:
            raise ValueError("Use an owner-only regular file for the selected peer.")
        pin = f.read(7)
    if len(pin) != 6 or pin in (bytes(6), bytes([255]) * 6):
        raise ValueError("The private peer file must contain six valid raw address bytes.")
    content = "#pragma once\n#define OMNI_PINNED_MOUSE2_BYTES " + ",".join(str(x) for x in pin) + "\n"
    dest.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if dest.is_symlink():
        raise ValueError("Refusing a symlink for a private compiler input.")
    tmp = dest.with_name(dest.name + ".new")
    with os.fdopen(os.open(tmp, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "w") as f:
        f.write(content)
    os.replace(tmp, dest)


if __name__ == "__main__":
    try:
        generate(*sys.argv[1:])
    except (OSError, ValueError) as e:
        # Do not echo source data, filesystem exception paths or compiler input.
        sys.exit(str(e) if isinstance(e, ValueError) else "Could not read/write the private build input.")
