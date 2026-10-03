"""Export exact pinned Git objects, including available vendor submodules.

Unlike the HEAD-consumer lane, this never substitutes current files for a pin.
Initialize the same vendor submodules required for ordinary consumer builds.
Missing required dependencies fail configuration rather than falling back to HEAD.
"""
from configparser import ConfigParser
import io
from pathlib import Path
import subprocess
import tarfile


def export(repository, revision, destination):
    archive = subprocess.check_output(['git', '-C', str(repository), 'archive', revision])
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        stream.extractall(destination, filter='data')
    tree = subprocess.check_output(
        ['git', '-C', str(repository), 'ls-tree', '-rz', revision])
    for entry in tree.split(b'\0'):
        if not entry:
            continue
        metadata, path = entry.split(b'\t', 1)
        mode, _, commit = metadata.split()
        child = repository / path.decode()
        if mode == b'160000' and (child / '.git').exists():
            export(child, commit.decode(), destination / path.decode())


def prepare(root, starter, work):
    wrap = ConfigParser()
    wrap.read(root / 'starters' / starter / 'subprojects/daveos.wrap')
    revision = wrap['wrap-git']['revision']
    if len(revision) != 40 or any(c not in '0123456789abcdef' for c in revision):
        raise ValueError('Starter must pin a full Git commit hash')
    # CI fetches the published pin explicitly; local full checkouts have it.
    subprocess.run(['git', '-C', str(root), 'cat-file', '-e', revision + '^{commit}'], check=True)
    destination = work / 'dependency' / revision
    marker = destination / '.export-complete'
    if not marker.exists():
        export(root, revision, destination)
        marker.write_text(revision + '\n')
    return destination
