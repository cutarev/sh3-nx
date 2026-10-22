#!/usr/bin/env python3
"""launcher_assets.py SPLASH.png: writes the launcher's images (src/d3d8/launcher.c) as BMP, which SDL loads with no
decoder: assets/splash.bmp (the warning, 1280x720), assets/logo.bmp (docs/logo.png with alpha) and assets/font.bmp
(ASCII 32-126 in a 16x6 grid of 40x80 cells, baseline at 60, gray = coverage; Source Code Pro, SIL OFL).
Needs ImageMagick."""
import subprocess, sys
font = '/usr/share/fonts/adobe-source-code-pro-fonts/SourceCodePro-Medium.otf'
subprocess.run(['magick', sys.argv[1], '-resize', '1280x720!', '-type', 'TrueColor', 'BMP3:assets/splash.bmp'], check=True)
subprocess.run(['magick', 'docs/logo.png', '-define', 'bmp:format=bmp4', 'BMP:assets/logo.bmp'], check=True)
cmd = ['magick', '-size', '640x480', 'xc:black', '-font', font, '-pointsize', '60', '-fill', 'white']
for i in range(95):
    c = chr(32 + i)
    cmd += ['-annotate', f'+{i % 16 * 40 + 2}+{i // 16 * 80 + 60}', {'%': '%%', '\\': '\\\\', '@': '\\@'}.get(c, c)]
subprocess.run(cmd + ['-colorspace', 'Gray', '-type', 'Palette', '-depth', '8', '-compress', 'None', 'BMP3:assets/font.bmp'], check=True)
