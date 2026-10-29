"""Deterministic signals encoded by FFmpeg, never sent to a speaker."""
from pathlib import Path
import math,struct,subprocess,sys,wave,shutil
root=Path(sys.argv[1]);root.mkdir(parents=True,exist_ok=True)
with wave.open(str(root/'tone.wav'),'wb') as w:
    w.setnchannels(2);w.setsampwidth(2);w.setframerate(44100)
    w.writeframes(b''.join(struct.pack('<hh',round(12000*math.sin(2*math.pi*1000*i/44100)),round(6000*math.sin(2*math.pi*2000*i/44100))) for i in range(88200)))
ffmpeg=shutil.which('ffmpeg')
if not ffmpeg: raise RuntimeError('FFmpeg is required to generate codec fixtures')
for ext,codec in [('mp3','libmp3lame'),('flac','flac'),('ogg','libvorbis')]:
    subprocess.run([ffmpeg,'-v','error','-y','-i',str(root/'tone.wav'),'-c:a',codec,str(root/('tone.'+ext))],check=True)
