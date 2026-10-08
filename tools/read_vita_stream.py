"""VITA5 — live read of the PS Vita UVC/UAC stream on this PC.
# SPDX-License-Identifier: GPL-3.0-or-later

Documents exactly what the boilerplate app must do:
  1. Enumerate video capture devices (find the Vita by name), list its supported
     UVC formats/resolutions (the frame descriptors the payload must probe).
  2. Capture real frames, measure achieved FPS, save one frame.
  3. Capture real audio from "Vita USB Stream", confirm sample rate/channels.
"""
import json, time, sys

def list_video_devices():
    from pygrabber.dshow_graph import FilterGraph
    g = FilterGraph()
    names = g.get_input_devices()
    return g, names

def device_formats(g, idx):
    """Supported media types (resolutions/subtypes) for a DirectShow video device."""
    try:
        g.add_video_input_device(idx)
        # get_input_devices already added; query formats via the device
    except Exception:
        pass
    try:
        fmts = g.get_input_device().formats if hasattr(g.get_input_device(), "formats") else None
    except Exception:
        fmts = None
    return fmts

def capture_video(idx, seconds=3.0):
    import cv2
    # Try native resolution first via MSMF, then DSHOW
    for backend in (cv2.CAP_MSMF, cv2.CAP_DSHOW):
        cap = cv2.VideoCapture(idx, backend)
        if cap.isOpened():
            # request the Vita's max (1280x720) to see what it hands back
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
            w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            fps_reported = cap.get(cv2.CAP_PROP_FPS)
            frames = 0
            t0 = time.time()
            first = None
            while time.time() - t0 < seconds:
                ok, frame = cap.read()
                if not ok:
                    break
                if first is None:
                    first = frame
                frames += 1
            dt = time.time() - t0
            cap.release()
            if first is not None:
                import cv2 as _cv2
                _cv2.imwrite(f"vita_frame_{w}x{h}.png", first)
            return {
                "backend": "MSMF" if backend == cv2.CAP_MSMF else "DSHOW",
                "w": w, "h": h,
                "fps_reported": fps_reported,
                "frames_read": frames,
                "elapsed_s": round(dt, 3),
                "fps_actual": round(frames / dt, 2) if dt > 0 else 0,
                "frame_shape": list(first.shape) if first is not None else None,
            }
    return {"error": "could not open video"}

def capture_audio(name_substr="Vita USB Stream", seconds=2.0):
    import sounddevice as sd
    import numpy as np
    dev = None
    for i, d in enumerate(sd.query_devices()):
        if name_substr in d["name"] and d["max_input_channels"] > 0 and d["hostapi"] == 2:
            dev = (i, d)
            break
    if dev is None:
        # fallback: any hostapi with the name
        for i, d in enumerate(sd.query_devices()):
            if name_substr in d["name"] and d["max_input_channels"] > 0:
                dev = (i, d)
                break
    if dev is None:
        return {"error": "Vita USB Stream audio device not found"}
    i, d = dev
    sr = int(d["default_samplerate"])
    ch = min(2, d["max_input_channels"])
    rec = sd.rec(int(seconds * sr), samplerate=sr, channels=ch, device=i, dtype="int16")
    sd.wait()
    peak = int(np.max(np.abs(rec))) if rec is not None and rec.size else 0
    rms = float(np.sqrt(np.mean(rec.astype("float64") ** 2))) if rec.size else 0.0
    return {
        "device_index": i,
        "name": d["name"],
        "hostapi": d["hostapi"],
        "samplerate": sr,
        "channels": ch,
        "sample_dtype": "int16",
        "samples_captured": int(rec.shape[0]),
        "peak_amplitude": peak,
        "rms": round(rms, 2),
        "has_signal": peak > 0,
    }

if __name__ == "__main__":
    out = {}
    g, names = list_video_devices()
    out["video_device_names"] = names
    # find Vita camera index by name heuristics
    vita_idx = None
    for i, n in enumerate(names):
        if "Vita" in n or "UDCD" in n or "UVC" in n.lower() or "PSV" in n:
            vita_idx = i
            break
    out["vita_video_index_guess"] = vita_idx
    # Capture from the guessed index, else every index until one yields 960x544-ish
    targets = [vita_idx] if vita_idx is not None else list(range(len(names)))
    out["video_capture"] = []
    for idx in targets:
        try:
            r = capture_video(idx)
            r["index"] = idx
            r["device_name"] = names[idx] if idx < len(names) else "?"
            out["video_capture"].append(r)
        except Exception as e:
            out["video_capture"].append({"index": idx, "error": repr(e)})
    try:
        out["audio_capture"] = capture_audio()
    except Exception as e:
        out["audio_capture"] = {"error": repr(e)}
    print(json.dumps(out, indent=2, default=str))
