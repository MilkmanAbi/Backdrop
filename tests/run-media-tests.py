"""Run from the repo root: python tests/run-media-tests.py path/to/Rainy.mp4 work-dir."""
import json
import subprocess
import sys
from pathlib import Path

source = Path(sys.argv[1]).resolve()
work = Path(sys.argv[2]).resolve()
work.mkdir(parents=True, exist_ok=True)
exe = Path("tests/media-smoke.exe").resolve()


def run(args, ok=True):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=180)
    if (result.returncode == 0) != ok:
        raise AssertionError(f"{args}: {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def inspect(path):
    data = json.loads(run(["ffprobe", "-v", "error", "-count_frames", "-show_streams", "-of", "json", path]))
    stream = data["streams"][0]
    assert stream["codec_name"] == "h264"
    assert stream["width"] % 2 == stream["height"] % 2 == 0
    run(["ffmpeg", "-v", "error", "-xerror", "-i", path, "-f", "null", "-"])
    return stream


original = json.loads(run(["ffprobe", "-v", "error", "-count_frames", "-show_streams", "-of", "json", source]))["streams"][0]
for fit in range(4):
    output = work / f"rainy-{fit}.mp4"
    run([exe, source, output, 1920, 1080, fit])
    stream = inspect(output)
    assert stream["nb_read_frames"] == original["nb_read_frames"]
    assert abs(float(stream["duration"]) - float(original["duration"])) < 0.1
    print(f"PASS Rainy fit={fit}: {stream['width']}x{stream['height']}, {stream['nb_read_frames']} frames", flush=True)

output = work / "software.mp4"
run([exe, source, output, 1280, 720, 0, "software"])
assert inspect(output)["nb_read_frames"] == original["nb_read_frames"]
print("PASS software decoder + encoder", flush=True)

for ext in ["gif", "webp"]:
    fixture = work / f"animation.{ext}"
    run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=10:duration=0.5", fixture])
    output = work / f"{ext}.mp4"
    run([exe, fixture, output, 640, 360, 0])
    assert int(inspect(output)["nb_read_frames"]) >= 5
    print(f"PASS animated {ext}", flush=True)

destination = work / "preserved.mp4"
sentinel = b"existing destination must survive"
destination.write_bytes(sentinel)
run([exe, source, destination, 640, 360, 0, "cancel"])
assert destination.read_bytes() == sentinel
run([exe, source, destination, 1920, 1080, 0, "cancel-late"])
assert destination.read_bytes() == sentinel
run([exe, source, destination, 0, 360, 0], ok=False)
assert destination.read_bytes() == sentinel
corrupt = work / "corrupt.mp4"
corrupt.write_bytes(b"not a video")
run([exe, corrupt, destination, 640, 360, 0], ok=False)
assert destination.read_bytes() == sentinel
run([exe, work / "missing.mp4", destination, 640, 360, 0], ok=False)
assert destination.read_bytes() == sentinel
assert not list(work.glob("*.part.mp4"))
print("PASS immediate/in-flight cancellation, invalid dimensions, corrupt/missing input; destination preserved; no partial files", flush=True)
