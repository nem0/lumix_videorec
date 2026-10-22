# Video Recorder Plugin

Records the **game view** or the **scene view** of Lumix Studio to an MP4 file using the hardware encoder in NVIDIA GPUs (NVENC). It talks to the driver directly, there is no ffmpeg or other external tool involved, and frames never leave the GPU.

**The code is made mostly by AI, so beware!**

## Requirements and limitations

- Windows 10 20H1 or later (NVENC's D3D12 interface) and an NVIDIA GPU with a driver 531.61 or newer. `nvEncodeAPI64.dll` is loaded at runtime, the plugin builds without an NVIDIA SDK.
- **AV1** (the default, royalty free) needs an RTX 40 series GPU or newer. H.264 and HEVC are supported by older GPUs too (not tested on them).
- The DX12 renderer backend, NVENC runs on the renderer's own D3D12 device.
- A Lumix Engine static plugin build.
- The scene view is recorded only while its window is the visible tab (there is no off-screen fallback for it, the game view has one), and a resize during the recording drops frames.
- MP4 only (no WebM or MKV), no audio track.

## Installation

See https://nem0.github.io/LumixEngine/plugins.html

## Usage

1. Open **Window > Video recorder**.
2. Start the game in Studio (recording is meant for game mode), set the options and press **Start recording**, then **Stop recording**.

| Option | Description |
| --- | --- |
| Output file | Path without extension, relative to the project directory unless absolute. `.mp4` is added. |
| Source | Game view (default) or Scene view. The scene view is recorded at the current size of its window, so the Scene View window has to be the visible tab and must not be resized while recording. Width and height are ignored for it. |
| Codec | AV1, H.264 or HEVC. |
| Width / Height | (`Custom` preset only) Resolution of the game view video, rounded down to even numbers. The game view is rendered at this size while recording, its window does not need to be open or visible. |
| FPS | (`Custom` preset only) Frame rate of the video and of the fixed simulation step. |
| Record every N-th frame | Capture one frame every N simulation steps, starting with the first frame. Defaults to 1 (every frame). Higher values speed up playback by N while keeping the video FPS and fixed simulation step unchanged. Available with all presets; saved as `videorec_frame_interval`. |
| Quality | (`Custom` preset only) 1-51, higher is better and makes bigger files. It is NVENC's constant quality level (CQ) inverted, the saved `videorec_quality` setting is the CQ itself (`52 - quality`, lower is better). |
| Encoder effort | (`Custom` preset only) NVENC preset P1-P7, not the target quality (that is Quality). Higher spends more GPU time to compress better at the same quality, 1 is the fastest, 7 the slowest. Saved as `videorec_preset`. |

The **Preset** combo (top of the window) sets the resolution, frame rate, quality and encoder effort at once (`Draft 720p@30`, `1080p@30`, `HQ 1080p@60`, `HQ 4K@60`, `Max 4K@60`), the codec and the output file are kept. The Width, FPS, Quality and Encoder effort controls below are shown only when the preset is `Custom` (also picked on load when the saved values match no preset). Resolution only matters for the game view, the scene view is recorded at the size of its window.

Settings are saved with Studio's settings (`videorec_*` in `studio.ini`). The recorder is also registered as the action `videorec_record`, it toggles recording and can be bound to a shortcut in Studio's shortcut editor.

## How it works

- While recording, the engine runs with a **fixed time step** (`Engine::setFixedTimeDelta`, `1 / FPS`) and one frame is captured every N steps, as set by **Record every N-th frame**. At the default of 1, the video is smooth even when Studio can't render in real time. Recording a heavy scene just takes longer than the video lasts.
- The recorder is a `RenderPlugin`. Its `renderEnd` hook (called at the end of every frame, after the UI, film grain and 2D drawing) gets the final image of the recorded view's pipeline and queues the encoding on the render thread.
- A small compute pass converts the image from RGBA8 (sRGB) to BGRA8, the result is copied to a ring of 6 textures registered in NVENC, and NVENC encodes them through its D3D12 interface, waiting on the fence of the renderer frame. A worker thread waits for NVENC's output fence and writes the bitstream to the file.
- If the encoder falls behind, the simulation is paused until it catches up, frames are not dropped. The same happens if the recorded view does not render (e.g. its window is hidden).
- The encoder uses constant quality VBR, no B-frames, 4:2:0, 8 bit, a key frame every two seconds.

## Output files

One MP4 file with a single video track (`av01`, `avc1` or `hvc1`), written by a small muxer (`Mp4Writer` in `src/videorec.cpp`). The samples are streamed to the file while recording and the index (`moov`) is appended when the recording stops, so a recording that crashes before that is not playable. The sequence header / SPS / PPS are taken from the encoder output and stored in the sample description. Constant frame rate, no B-frames, no audio.
