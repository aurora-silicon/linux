#!/usr/bin/env python3
"""Extend the managed native Pro session to the paired release profile."""
import hashlib
import os
from pathlib import Path
import pwd
import stat
import sys
import tempfile

home = Path(pwd.getpwnam(sys.argv[1]).pw_dir)
directory = home / '.local/libexec'
expected = {
    'native-pro-command': '39aa4478c0332e2cb6a7cd9abfb5834d4a3a95365d41e40a7494647845ee8d59',
    'native-pro-desktop': '02133ab43b1cecbea6f6a4ab59df31a6a73b430750b844c955d56b6ac5b548c8',
    'chonkstep-native-pro-session': '1d222fad2a891f83292d467a33cfca42914bb1a46d1715e0c61ea6e245f1d4f8',
}
for name, digest in expected.items():
    path = directory / name
    if not path.is_file() or path.is_symlink():
        continue
    info = path.stat()
    if info.st_uid != pwd.getpwnam(sys.argv[1]).pw_uid:
        continue
    original = path.read_bytes()
    if hashlib.sha256(original).hexdigest() != digest:
        print(f'Keeping custom session {path}')
        continue
    text = original.decode()
    if name == 'native-pro-desktop':
        anchor = 'case "$(uname -r)" in\n'
        assert text.count(anchor) == 1
        text = text.replace(anchor, '''if /opt/mesa-m3/native-pro/libexec/mesa-m3-pro-check &&
   [[ -f "$HOME/.config/chonkstep/native-pro-enabled" ]]; then
 exec uwsm start -g -1 -e -D chonkstep -N chonkstep-native-pro -- "$HOME/.local/libexec/chonkstep-native-pro-session" --display=dcp "$@"
fi
''' + anchor)
    elif name == 'native-pro-command':
        anchor = 'exec "$@"'
        assert text.count(anchor) == 1
        text = text.replace(anchor, '. /opt/mesa-m3/native-pro/libexec/mesa-m3-pro-session-env\n\n' + anchor)
    else:
        anchor = 'identity=$(/opt/mesa-m3/libexec/mesa-m3-abi-check'
        assert text.count(anchor) == 1
        text = text.replace(anchor, '''if /opt/mesa-m3/native-pro/libexec/mesa-m3-pro-check; then
 K=/opt/mesa-m3/pro-vulkan
fi
''' + anchor)
        text = text.replace('[ "$K" = "$V" ] || ICD="$K/private-icd.json"',
                            '[ "$K" = "$V" ] || [ "$K" = /opt/mesa-m3/pro-vulkan ] || ICD="$K/private-icd.json"')
        anchor = 'exec /home/chrisk/src/chonkstep/scripts/wayland-session.sh'
        assert text.count(anchor) == 1
        text = text.replace(anchor, '. /opt/mesa-m3/native-pro/libexec/mesa-m3-pro-session-env\n\n' + anchor)
    backup = path.with_name(path.name + '.aurora-' + digest[:12] + '.saved')
    if not backup.exists():
        backup.write_bytes(original)
        os.chown(backup, info.st_uid, info.st_gid)
        os.chmod(backup, stat.S_IMODE(info.st_mode))
    else:
        assert backup.read_bytes() == original
    fd, temporary = tempfile.mkstemp(prefix=name + '.', dir=directory)
    try:
        with os.fdopen(fd, 'w') as output:
            output.write(text)
            output.flush()
            os.fsync(output.fileno())
        os.chown(temporary, info.st_uid, info.st_gid)
        os.chmod(temporary, stat.S_IMODE(info.st_mode))
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(f'Updated managed session {path}; previous file is {backup}')
