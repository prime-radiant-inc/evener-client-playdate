#!/usr/bin/env python3
"""Composite an editable onboarding film from supplied hardware/browser captures."""
from __future__ import annotations

import argparse
from functools import lru_cache
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
W, H = 1920, 1080
INK = (21, 25, 25)
YELLOW = (255, 211, 42)
PAPER = (244, 242, 233)
WHITE = (255, 255, 253)
MUTED = (139, 146, 141)
FONT = '/System/Library/Fonts/Avenir Next.ttc'
STILLS = {'.png', '.jpg', '.jpeg', '.webp'}


@lru_cache(80)
def font(size, weight='demi'):
    return ImageFont.truetype(FONT, size, index={'bold': 0, 'demi': 2, 'regular': 7}[weight])


def text(im, value, xy, size=30, fill=INK, weight='demi'):
    ImageDraw.Draw(im).text(xy, value, font=font(size, weight), fill=fill, anchor='lt')


def wrap(im, value, width, size, weight='demi'):
    draw = ImageDraw.Draw(im)
    lines = []
    for paragraph in value.split('\n'):
        line = ''
        for word in paragraph.split():
            candidate = f'{line} {word}'.strip()
            if line and draw.textlength(candidate, font=font(size, weight)) > width:
                lines.append(line)
                line = word
            else:
                line = candidate
        lines.append(line)
    return lines


def paragraph(im, value, xy, width, size, fill=INK, weight='demi', leading=1.25):
    x, y = xy
    for line in wrap(im, value, width, size, weight):
        text(im, line, (x, y), size, fill, weight)
        y += size * leading
    return y


def resolve(base, value):
    if not value:
        return None
    path = Path(value).expanduser()
    return path if path.is_absolute() else base / path


@lru_cache(32)
def probe(path):
    result = subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
        'stream=width,height,duration:format=duration', '-of', 'json', path])
    data = json.loads(result)
    stream = data['streams'][0]
    duration = stream.get('duration') or data.get('format', {}).get('duration', 0)
    return int(stream['width']), int(stream['height']), float(duration)


class Source:
    """Decode one bounded frame at a time; never loop a recorded action."""
    def __init__(self, spec, target, base, fps, preview):
        self.spec, self.target, self.fps = spec, target, fps
        self.path = resolve(base, spec.get('path'))
        self.missing = not self.path or not self.path.is_file()
        self.proc = None
        self.diagnostics = None
        self.frame_index = -1
        self.last = None
        self.held = False
        self.still = False
        self.start = float(spec.get('in', 0))
        self.speed = float(spec.get('speed', 1))
        if self.start < 0 or self.speed <= 0:
            raise ValueError('Media in must be nonnegative and speed must be positive')
        if self.missing:
            if not preview:
                raise ValueError(f'Missing {target} capture: {self.path}')
            self.duration = 0
            return
        self.still = self.path.suffix.lower() in STILLS
        if self.still:
            im = Image.open(self.path).convert('RGB')
            self.width, self.height = im.size
            self.duration = math.inf
        else:
            self.width, self.height, source_duration = probe(str(self.path))
            available = max(0, source_duration - self.start)
            self.duration = min(float(spec.get('duration', available)), available)
            if self.duration <= 0:
                raise ValueError(f'No video remains after trim: {self.path}')
        crop = spec.get('crop', [0, 0, self.width, self.height])
        x, y, width, height = map(int, crop)
        if x < 0 or y < 0 or min(width, height) <= 0 or x + width > self.width or y + height > self.height:
            raise ValueError(f'Crop exceeds capture dimensions: {self.path}')
        self.crop = (x, y, width, height)
        if target == 'device' and abs(width / height - 5 / 3) > .005:
            raise ValueError('Device crop must be exactly the Playdate display, at 5:3 aspect ratio')
        if target == 'device' and self.still and not preview:
            raise ValueError('Final device scenes require actual hardware video')
        if self.still:
            self.last = im.crop((x, y, x + width, y + height))

    def close(self):
        if self.proc:
            # Closing our read end releases a decoder blocked on unconsumed frames.
            self.proc.stdout.close()
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            self.proc = None
        if self.diagnostics is not None:
            self.diagnostics.close()
            self.diagnostics = None

    def frame(self, seconds):
        if self.missing:
            return None
        if self.still:
            return self.last
        source_time = seconds * self.speed
        self.held = source_time >= self.duration
        if self.held and not self.spec.get('hold_last'):
            raise ValueError(f'Capture overrun requires hold_last=true: {self.path}')
        target = min(int(source_time * self.fps), max(0, math.ceil(self.duration * self.fps) - 1))
        if self.proc is None or target < self.frame_index or target - self.frame_index > self.fps * 3:
            self.close()
            x, y, width, height = self.crop
            self.decode_size = (800, 480) if self.target == 'device' else (width, height)
            dw, dh = self.decode_size
            scale = 'neighbor' if self.target == 'device' else 'lanczos'
            # Mirror writes sparse updates. Expand held frames before trimming so
            # a cut into an unchanged screen preserves the screen visible then.
            trim_start = self.start + target / self.fps
            filters = (f'crop={width}:{height}:{x}:{y},scale={dw}:{dh}:flags={scale},'
                       f'fps={self.fps},trim=start={trim_start},setpts=PTS-STARTPTS')
            # A file retains decoder errors without filling an unread stderr pipe.
            self.diagnostics = tempfile.TemporaryFile()
            self.proc = subprocess.Popen([
                'ffmpeg', '-hide_banner', '-loglevel', 'error', '-xerror', '-i', str(self.path),
                '-an', '-vf', filters, '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'],
                stdout=subprocess.PIPE, stderr=self.diagnostics)
            self.frame_index = target - 1
        count = self.decode_size[0] * self.decode_size[1] * 3
        while self.frame_index < target:
            raw = self.proc.stdout.read(count)
            if len(raw) != count:
                self.diagnostics.seek(0)
                detail = self.diagnostics.read().decode('utf-8', errors='replace').strip()
                self.close()
                raise ValueError(
                    f'Decoder ended before frame {target} of {self.path}'
                    + (f'\n{detail}' if detail else ''))
            self.last = Image.frombytes('RGB', self.decode_size, raw)
            self.frame_index += 1
        return self.last


class Film:
    def __init__(self, path, preview=False):
        self.path = Path(path).resolve()
        self.base = self.path.parent
        self.manifest = json.loads(self.path.read_text())
        self.preview = preview or bool(self.manifest.get('preview', False))
        self.fps = int(self.manifest.get('fps', 30))
        if not 1 <= self.fps <= 60:
            raise ValueError('fps must be between 1 and 60')
        self.scenes = self.manifest['scenes']
        self.duration = 0
        self.sources = {}
        self.active_scene = None
        self.stills = {}
        self.audio = []
        for number, scene in enumerate(self.scenes):
            scene['start'] = self.duration
            scene['number'] = number + 1
            duration = float(scene['duration'])
            if duration <= 0:
                raise ValueError('Scene duration must be positive')
            layout = scene.get('layout', 'split')
            required = {'split': ('device', 'browser'), 'device': ('device',), 'browser': ('browser',), 'title': ()}[layout]
            for target in required:
                spec = scene.get(target, {})
                source = Source(spec, target, self.base, self.fps, self.preview)
                if not self.preview and duration * source.speed > source.duration + 1 / self.fps and not spec.get('hold_last'):
                    raise ValueError(f'{scene["id"]}/{target}: capture is shorter than scene; trim scene or set hold_last=true')
                self.sources[(number, target)] = source
            for track in scene.get('audio', []):
                self.audio.append(dict(track, at=scene['start'] + float(track.get('at', 0))))
            self.duration += duration
        self.audio.extend(self.manifest.get('audio', []))
        for track in self.audio:
            track['resolved'] = resolve(self.base, track.get('path'))
            path = track['resolved']
            if (not path or not path.is_file()) and not self.preview:
                raise ValueError(f'Missing audio source: {path}')
        logo_path = resolve(self.base, self.manifest.get('logo', '../assets/primeradiant-logo.png'))
        if not logo_path or not logo_path.is_file():
            raise ValueError('Supply the official Prime Radiant logo')
        logo = Image.open(logo_path).convert('RGBA')
        self.logo = logo.resize((226, round(226 * logo.height / logo.width)), Image.Resampling.LANCZOS)

    def close(self):
        for source in self.sources.values():
            source.close()
        self.stills.clear()

    def at(self, seconds):
        for index, scene in enumerate(self.scenes):
            if seconds < scene['start'] + scene['duration']:
                return index, scene, seconds - scene['start']
        return len(self.scenes) - 1, self.scenes[-1], self.scenes[-1]['duration'] - 1 / self.fps

    def media(self, im, number, target, seconds, box):
        x, y, width, height = box
        draw = ImageDraw.Draw(im)
        source = self.sources[(number, target)]
        frame = source.frame(seconds)
        draw.rounded_rectangle((x - 12, y - 12, x + width + 12, y + height + 12), 17, fill=INK)
        if target == 'device':
            draw.rectangle((x - 12, y + 28, x - 7, y + height - 28), fill=YELLOW)
        if frame is None:
            draw.rectangle((x, y, x + width, y + height), fill=(46, 51, 49))
            paragraph(im, 'REAL CAPTURE\nNOT SUPPLIED', (x + 40, y + height / 2 - 55), width - 80, 36, PAPER)
            text(im, 'LAYOUT PROOF ONLY', (x + 40, y + height - 65), 22, YELLOW)
            return box
        scale = min(width / frame.width, height / frame.height)
        size = (round(frame.width * scale), round(frame.height * scale))
        origin = (x + (width - size[0]) // 2, y + (height - size[1]) // 2)
        method = Image.Resampling.NEAREST if target == 'device' else Image.Resampling.LANCZOS
        key = (number, target, size, method)
        if source.still and key in self.stills:
            scaled = self.stills[key]
        else:
            scaled = frame.resize(size, method)
            if source.still:
                self.stills[key] = scaled
        im.paste(scaled, origin)
        if source.held:
            text(im, 'RECORDED FRAME HELD', (x, y + height + 24), 18, MUTED)
        return (*origin, *size)

    def frame(self, seconds):
        number, scene, local = self.at(seconds)
        if number != self.active_scene:
            for (owner, _), source in self.sources.items():
                if owner == self.active_scene:
                    source.close()
            self.active_scene = number
        layout = scene.get('layout', 'split')
        dark = layout == 'title'
        im = Image.new('RGB', (W, H), INK if dark else PAPER)
        draw = ImageDraw.Draw(im)
        draw.rectangle((0, 0, W, 130), fill=INK)
        text(im, 'EVENER / PLAYDATE', (72, 50), 26, PAPER)
        text(im, 'ONBOARDING', (420, 53), 20, MUTED)
        im.paste(self.logo, (1622, 30), self.logo)
        draw.rectangle((72, 128, 216, 133), fill=YELLOW)
        badge = f'{scene["number"]:02d} / {len(self.scenes):02d}'
        text(im, badge, (72, 169), 24, YELLOW if dark else INK)
        text(im, scene.get('chapter', '').upper(), (230, 172), 22, MUTED if dark else (96, 104, 96))
        regions = {}
        if layout == 'title':
            paragraph(im, scene['title'], (72, 296), 1580, 104, PAPER, 'bold', 1.08)
            paragraph(im, scene.get('instruction', ''), (78, 594), 1480, 41, PAPER, 'regular', 1.3)
            text(im, scene.get('cue', ''), (78, 845), 26, YELLOW)
        elif layout == 'device':
            paragraph(im, scene['title'], (72, 316), 505, 66, INK, 'bold', 1.12)
            paragraph(im, scene.get('instruction', ''), (76, 595), 490, 30, INK, 'regular', 1.35)
            text(im, 'PLAYDATE / HARDWARE RECORDING', (648, 200), 20, (91, 101, 94))
            regions['device'] = self.media(im, number, 'device', local, (648, 242, 1200, 720))
            text(im, scene.get('cue', ''), (76, 871), 25, INK)
        elif layout == 'browser':
            paragraph(im, scene['title'], (72, 225), 1640, 56, INK, 'bold')
            text(im, 'BROWSER / CAPTURE', (84, 326), 20, (91, 101, 94))
            regions['browser'] = self.media(im, number, 'browser', local, (84, 365, 1240, 590))
            paragraph(im, scene.get('instruction', ''), (1400, 402), 445, 35, INK, 'demi', 1.3)
            paragraph(im, scene.get('cue', ''), (1400, 801), 420, 26, INK, 'regular')
        else:
            paragraph(im, scene['title'], (72, 225), 1640, 56, INK, 'bold')
            text(im, 'PLAYDATE / HARDWARE RECORDING', (84, 326), 20, (91, 101, 94))
            text(im, 'BROWSER / CAPTURE', (1008, 326), 20, (91, 101, 94))
            regions['device'] = self.media(im, number, 'device', local, (84, 380, 800, 480))
            regions['browser'] = self.media(im, number, 'browser', local, (1008, 365, 828, 520))
            paragraph(im, scene.get('instruction', ''), (84, 925), 1670, 31, INK, 'regular', 1.25)
        for highlight in scene.get('highlights', []):
            if highlight['target'] not in regions or not float(highlight.get('from', 0)) <= local <= float(highlight.get('to', scene['duration'])):
                continue
            x, y, width, height = regions[highlight['target']]
            rx, ry, rw, rh = highlight['rect']
            box = (round(x + rx * width), round(y + ry * height), round(x + (rx + rw) * width), round(y + (ry + rh) * height))
            draw.rounded_rectangle(box, 8, outline=YELLOW, width=4 + int(math.sin(local * 2) > .5))
        if scene.get('edited_waits'):
            text(im, 'WAITS EDITED FOR LENGTH', (1090, 174), 18, MUTED if dark else (96, 104, 96))
        draw.line((72, 1046, 1848, 1046), fill=(73, 80, 75) if dark else (196, 200, 189), width=2)
        draw.line((72, 1046, 72 + 1776 * min(1, seconds / self.duration), 1046), fill=YELLOW if dark else INK, width=4)
        text(im, 'primeradiant.com', (72, 1009), 21, YELLOW if dark else INK)
        text(im, 'Experimental native client', (1455, 1009), 21, MUTED if dark else (96, 104, 96))
        if self.preview:
            draw.rounded_rectangle((630, 1000, 1290, 1036), 7, fill=(145, 42, 37))
            text(im, 'LAYOUT PROOF / CAPTURES NOT VERIFIED', (653, 1008), 18, WHITE)
        return im


def timecode(seconds):
    total = round(seconds * 1000)
    hours, total = divmod(total, 3600000)
    minutes, total = divmod(total, 60000)
    sec, ms = divmod(total, 1000)
    return f'{hours:02}:{minutes:02}:{sec:02},{ms:03}'


def narration_cues(film, path):
    cues = []
    for scene in film.scenes:
        narration = scene.get('narration', '')
        if narration:
            cues.append(f'{len(cues) + 1}\n{timecode(scene["start"])} --> {timecode(scene["start"] + scene["duration"])}\n{narration}\n')
    path.write_text('\n'.join(cues))


def chapter_metadata(film, path):
    lines = [';FFMETADATA1']
    for scene in film.scenes:
        title = scene.get('chapter', scene.get('id', 'Scene'))
        for character in ('\\', '=', ';', '#', '\n'):
            title = title.replace(character, '\\' + character)
        lines.extend([
            '[CHAPTER]',
            'TIMEBASE=1/1000',
            f'START={round(scene["start"] * 1000)}',
            f'END={round((scene["start"] + scene["duration"]) * 1000)}',
            f'title={title}',
        ])
    path.write_text('\n'.join(lines) + '\n')


def mux_audio(film, silent, output):
    tracks = [track for track in film.audio if track['resolved'] and track['resolved'].is_file()]
    if not tracks:
        silent.replace(output)
        return
    command = ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error', '-i', str(silent)]
    for track in tracks:
        command += ['-i', str(track['resolved'])]
    filters = []
    for index, track in enumerate(tracks):
        trim = f'atrim=start={float(track.get("in", 0))}'
        if 'duration' in track:
            trim += f':duration={float(track["duration"])}'
        delay = round(float(track.get('at', 0)) * 1000)
        gain = float(track.get('gain_db', 0))
        filters.append(f'[{index + 1}:a]{trim},asetpts=PTS-STARTPTS,aresample=48000,aformat=channel_layouts=stereo,volume={gain}dB,adelay={delay}|{delay}[a{index}]')
    inputs = ''.join(f'[a{i}]' for i in range(len(tracks)))
    filters.append(f'{inputs}amix=inputs={len(tracks)}:normalize=0:duration=longest,apad,atrim=duration={film.duration},alimiter=limit=0.98:latency=1[mix]')
    command += ['-filter_complex', ';'.join(filters), '-map', '0:v:0', '-map', '[mix]', '-c:v', 'copy', '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', str(output)]
    subprocess.run(command, check=True)
    silent.unlink()


def render(film, output, scale):
    width, height = round(W * scale / 2) * 2, round(H * scale / 2) * 2
    output.parent.mkdir(parents=True, exist_ok=True)
    silent = output.with_name(output.stem + '.silent.mp4')
    command = ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
               '-s', f'{width}x{height}', '-r', str(film.fps), '-i', 'pipe:0', '-an', '-c:v', 'libx264',
               '-preset', 'fast', '-crf', '18', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(silent)]
    process = subprocess.Popen(command, stdin=subprocess.PIPE)
    try:
        total = math.ceil(film.duration * film.fps)
        for frame_number in range(total):
            frame = film.frame(frame_number / film.fps)
            if frame.size != (width, height):
                frame = frame.resize((width, height), Image.Resampling.LANCZOS)
            process.stdin.write(frame.tobytes())
            if frame_number % (film.fps * 10) == 0:
                print(f'{frame_number / film.fps:.0f} / {film.duration:.0f} seconds', flush=True)
        process.stdin.close()
        if process.wait() != 0:
            raise RuntimeError('FFmpeg video encoding failed')
        mux_audio(film, silent, output)
    except BaseException:
        process.terminate()
        process.wait()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=ROOT / 'onboarding-draft.mp4')
    parser.add_argument('--preview', action='store_true', help='Clearly mark layout proof and allow absent sources')
    parser.add_argument('--frames', type=float, nargs='+', help='Write proof PNGs at these film times instead of encoding')
    parser.add_argument('--scale', type=float, default=1, help='Output scale, e.g. 0.5 for a 960x540 draft')
    parser.add_argument('--validate', action='store_true', help='Check sources and scene durations without encoding')
    args = parser.parse_args()
    if not 0 < args.scale <= 1:
        parser.error('--scale must be greater than zero and at most one')
    film = Film(args.manifest, args.preview)
    try:
        print(f'{len(film.scenes)} scenes; {film.duration:.2f}s; {film.fps}fps; preview={film.preview}')
        if args.validate:
            return
        if args.frames:
            args.out.mkdir(parents=True, exist_ok=True)
            for seconds in args.frames:
                film.frame(seconds).save(args.out / f'frame-{seconds:06.2f}.png')
            narration_cues(film, args.out / 'narration-draft.srt')
        else:
            render(film, args.out, args.scale)
            narration_cues(film, args.out.with_suffix('.narration-draft.srt'))
            chapter_metadata(film, args.out.with_suffix('.chapters.ffmetadata'))
    finally:
        film.close()


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError) as exc:
        print(f'Onboarding render: {exc}', file=sys.stderr)
        sys.exit(2)
