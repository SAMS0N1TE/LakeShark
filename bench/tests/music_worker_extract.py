"""Compile the production playback loop with deterministic queue/output boundaries."""
from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text()
config=s[s.index('static std::atomic<unsigned> speed_permille'):s.index('static std::atomic<uint32_t> stop_serial')]
worker=s[s.index('static esp_err_t mono_to_stereo('):s.index('static void audio_task(')]
Path(sys.argv[2]).write_text(config+'\n'+worker)
