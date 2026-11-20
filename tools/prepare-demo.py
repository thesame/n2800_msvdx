#!/usr/bin/env python3
"""Convert a supplied movie to the conservative demo format; validate packets."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input')
    p.add_argument('output', type=Path)
    a = p.parse_args()
    if a.output.exists():
        p.error('output exists; refusing to replace it')
    subprocess.run(['ffmpeg', '-v', 'warning', '-i', a.input, '-vf', 'scale=1280:720,fps=25',
        '-c:v', 'libx264', '-threads', '4', '-preset', 'fast', '-profile:v', 'main',
        '-crf', '26', '-maxrate', '900k', '-bufsize', '320k', '-x264-params',
        'ref=2:bframes=0:weightp=0:slices=1:keyint=50:scenecut=0', '-c:a', 'aac',
        '-ac', '2', '-b:a', '96k', '-movflags', '+faststart', str(a.output)], check=True)
    packets = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams',
        'v:0', '-show_packets', '-show_entries', 'packet=size', '-of', 'json', str(a.output)]))['packets']
    largest = max(int(p['size']) for p in packets)
    if largest > 65535:
        raise SystemExit('packet exceeds prototype slice limit: ' + str(largest))
    print('Prepared exact-720p demo; maximum video packet bytes:', largest)


if __name__ == '__main__':
    main()
