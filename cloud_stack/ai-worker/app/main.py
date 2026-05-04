from __future__ import annotations

import io
import math
import os
import re
import struct
import wave
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

import httpx
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from pydantic import BaseModel

try:
    from openai import OpenAI
except ImportError:  # pragma: no cover - handled at runtime
    OpenAI = None


PUBLIC_ROOT = Path(os.getenv("PUBLIC_ROOT", "/srv/public")).resolve()
UPLOAD_DIR = Path(os.getenv("PUBLIC_UPLOAD_DIR", str(PUBLIC_ROOT / "uploads" / "devkit"))).resolve()
REPLY_DIR = Path(os.getenv("PUBLIC_REPLY_DIR", str(PUBLIC_ROOT / "audio_reply"))).resolve()
FIRMWARE_DIR = Path(os.getenv("PUBLIC_FIRMWARE_DIR", str(PUBLIC_ROOT / "firmware"))).resolve()
PUBLIC_BASE_URL = os.getenv("PUBLIC_BASE_URL", "http://127.0.0.1").rstrip("/")

AI_MOCK_MODE = os.getenv("AI_MOCK_MODE", "true").strip().lower() in {"1", "true", "yes", "on"}
OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "").strip()
OPENAI_TRANSCRIBE_MODEL = os.getenv("OPENAI_TRANSCRIBE_MODEL", "gpt-4o-mini-transcribe")
OPENAI_REASONING_MODEL = os.getenv("OPENAI_REASONING_MODEL", "gpt-4.1-mini")
OPENAI_TTS_MODEL = os.getenv("OPENAI_TTS_MODEL", "gpt-4o-mini-tts")
OPENAI_TTS_VOICE = os.getenv("OPENAI_TTS_VOICE", "coral")
OPENAI_TTS_INSTRUCTIONS = os.getenv(
    "OPENAI_TTS_INSTRUCTIONS",
    "Speak clearly, slowly, and warmly for a wearable assistive device.",
)
REPLY_AUDIO_SAMPLE_RATE_HZ = int(os.getenv("REPLY_AUDIO_SAMPLE_RATE_HZ", "16000"))
REPLY_AUDIO_TARGET_PEAK = int(os.getenv("REPLY_AUDIO_TARGET_PEAK", "22000"))

SAFE_NAME_RE = re.compile(r"[^A-Za-z0-9._-]+")

app = FastAPI(
    title="Mind2Matter AI Worker",
    description="Upload, orchestration, and audio generation service for the A08G/A09G cloud stack.",
    version="1.0.0",
)


class VoiceRequest(BaseModel):
    request_id: str
    device_id: str = "glasses01"
    mode: str = "qa"
    audio_url: Optional[str] = None
    image_url: Optional[str] = None
    text_prompt: Optional[str] = None
    dev_board_stub: bool = False
    fw_version: Optional[str] = None
    uptime_ms: Optional[int] = None


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def sanitize_name(value: str, fallback: str) -> str:
    cleaned = SAFE_NAME_RE.sub("_", value or "").strip("._")
    return cleaned or fallback


def ensure_runtime_dirs() -> None:
    for directory in (PUBLIC_ROOT, UPLOAD_DIR, REPLY_DIR, FIRMWARE_DIR):
        directory.mkdir(parents=True, exist_ok=True)


def public_url_for(relative_path: str) -> str:
    return f"{PUBLIC_BASE_URL}/{relative_path.lstrip('/')}"


def local_file_from_public_url(url: str) -> Optional[Path]:
    prefix = PUBLIC_BASE_URL + "/"
    if not url.startswith(prefix):
        return None

    relative_path = url[len(prefix):]
    candidate = (PUBLIC_ROOT / relative_path).resolve()
    try:
        candidate.relative_to(PUBLIC_ROOT)
    except ValueError:
        return None
    return candidate


async def read_url_bytes(url: str) -> bytes:
    local_path = local_file_from_public_url(url)
    if local_path is not None and local_path.exists():
        return local_path.read_bytes()

    async with httpx.AsyncClient(timeout=30.0, follow_redirects=True) as client:
        response = await client.get(url)
        response.raise_for_status()
        return response.content


def openai_client() -> Optional["OpenAI"]:
    if AI_MOCK_MODE or not OPENAI_API_KEY or OpenAI is None:
        return None
    return OpenAI(api_key=OPENAI_API_KEY)


def build_reasoning_prompt(request: VoiceRequest, transcript_text: str) -> str:
    lines = [
        "You are the cloud assistant for Mind2Matter smart glasses.",
        "Give concise answers that sound natural when read aloud.",
        f"Device ID: {request.device_id}",
        f"Mode: {request.mode}",
    ]
    if request.text_prompt:
        lines.append(f"User prompt: {request.text_prompt}")
    if transcript_text:
        lines.append(f"Transcript: {transcript_text}")
    elif request.dev_board_stub:
        lines.append("The request came from the dev-board stub and may not include real audio.")
    else:
        lines.append("No transcript was available. Infer the most helpful short reply from the metadata.")
    return "\n".join(lines)


def build_mock_answer(request: VoiceRequest, transcript_text: str) -> str:
    if request.mode in {"notice", "tts"} and request.text_prompt:
        return request.text_prompt.strip()
    if request.mode == "vision":
        if request.text_prompt:
            return f"Mock vision reply: I received the image and the prompt '{request.text_prompt}'."
        return "Mock vision reply: I received the image request and the cloud pipeline is healthy."
    if transcript_text:
        return f"Mock reply: I heard '{transcript_text}'."
    if request.dev_board_stub:
        return "Mock reply: the dev board request reached the cloud service successfully."
    return "Mock reply: the cloud service is running, but no transcript was available."


def clamp_pcm16(value: float) -> int:
    rounded = int(round(value))
    if rounded > 32767:
        return 32767
    if rounded < -32768:
        return -32768
    return rounded


def resample_pcm16_linear(samples: list[int], source_rate: int, target_rate: int) -> list[int]:
    if not samples or source_rate <= 0 or target_rate <= 0 or source_rate == target_rate:
        return samples

    output_count = max(1, int(round((len(samples) * target_rate) / source_rate)))
    if output_count == 1 or len(samples) == 1:
        return [samples[0]]

    result: list[int] = []
    step = (len(samples) - 1) / (output_count - 1)
    for index in range(output_count):
        source_pos = index * step
        left = int(source_pos)
        right = min(left + 1, len(samples) - 1)
        fraction = source_pos - left
        mixed = (samples[left] * (1.0 - fraction)) + (samples[right] * fraction)
        result.append(clamp_pcm16(mixed))
    return result


def write_mono_wav(path: Path, samples: list[int], sample_rate: int) -> None:
    with wave.open(str(path), "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(b"".join(struct.pack("<h", sample) for sample in samples))


def normalize_reply_wav(path: Path) -> dict:
    with wave.open(str(path), "rb") as wav_file:
        source_channels = wav_file.getnchannels()
        sample_width = wav_file.getsampwidth()
        source_rate = wav_file.getframerate()
        frame_count = wav_file.getnframes()
        raw_frames = wav_file.readframes(frame_count)

    if sample_width != 2 or source_channels < 1:
        raise ValueError(f"unsupported reply WAV format channels={source_channels} width={sample_width}")

    source_samples = struct.unpack(f"<{len(raw_frames) // 2}h", raw_frames)
    mono_samples: list[int] = []
    for offset in range(0, len(source_samples), source_channels):
        frame = source_samples[offset:offset + source_channels]
        if not frame:
            continue
        mono_samples.append(clamp_pcm16(sum(frame) / len(frame)))

    mono_samples = resample_pcm16_linear(mono_samples, source_rate, REPLY_AUDIO_SAMPLE_RATE_HZ)
    peak = max((abs(sample) for sample in mono_samples), default=0)
    if peak > REPLY_AUDIO_TARGET_PEAK > 0:
        scale = REPLY_AUDIO_TARGET_PEAK / peak
        mono_samples = [clamp_pcm16(sample * scale) for sample in mono_samples]
        peak = max((abs(sample) for sample in mono_samples), default=0)

    write_mono_wav(path, mono_samples, REPLY_AUDIO_SAMPLE_RATE_HZ)
    return {
        "source_rate_hz": source_rate,
        "source_channels": source_channels,
        "sample_rate_hz": REPLY_AUDIO_SAMPLE_RATE_HZ,
        "channels": 1,
        "frames": len(mono_samples),
        "peak_abs": peak,
    }


def write_mock_wav(path: Path) -> None:
    sample_rate = REPLY_AUDIO_SAMPLE_RATE_HZ
    duration_s = 1.2
    frequency_hz = 660.0
    amplitude = 10000
    frame_count = int(sample_rate * duration_s)

    with wave.open(str(path), "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)

        frames = bytearray()
        for index in range(frame_count):
            envelope = 1.0 if index < frame_count * 0.85 else 0.35
            sample = int(amplitude * envelope * math.sin((2.0 * math.pi * frequency_hz * index) / sample_rate))
            frames.extend(struct.pack("<h", sample))
        wav_file.writeframes(bytes(frames))


def transcribe_audio(client: "OpenAI", audio_bytes: Optional[bytes], request: VoiceRequest) -> str:
    if not audio_bytes:
        return ""

    audio_buffer = io.BytesIO(audio_bytes)
    audio_buffer.name = f"{sanitize_name(request.request_id, 'request')}.wav"
    transcript = client.audio.transcriptions.create(
        model=OPENAI_TRANSCRIBE_MODEL,
        file=audio_buffer,
        response_format="text",
    )
    text = getattr(transcript, "text", None)
    if isinstance(text, str) and text:
        return text
    if isinstance(transcript, str):
        return transcript
    return ""


def reason_over_request(client: "OpenAI", request: VoiceRequest, transcript_text: str) -> str:
    content = [
        {
            "type": "input_text",
            "text": build_reasoning_prompt(request, transcript_text),
        }
    ]

    if request.image_url:
        content.append(
            {
                "type": "input_image",
                "image_url": request.image_url,
                "detail": "low",
            }
        )

    response = client.responses.create(
        model=OPENAI_REASONING_MODEL,
        input=[
            {
                "role": "user",
                "content": content,
            }
        ],
    )
    answer = getattr(response, "output_text", "")
    if not answer:
        raise RuntimeError("OpenAI reasoning response did not include output_text.")
    return answer.strip()


def synthesize_speech(client: "OpenAI", text: str, destination: Path) -> None:
    with client.audio.speech.with_streaming_response.create(
        model=OPENAI_TTS_MODEL,
        voice=OPENAI_TTS_VOICE,
        input=text,
        instructions=OPENAI_TTS_INSTRUCTIONS,
        response_format="wav",
    ) as response:
        response.stream_to_file(destination)


@app.on_event("startup")
async def startup() -> None:
    ensure_runtime_dirs()


@app.get("/api/health")
async def health() -> dict:
    return {
        "ok": True,
        "mock_mode": AI_MOCK_MODE or not OPENAI_API_KEY,
        "public_base_url": PUBLIC_BASE_URL,
        "upload_dir": str(UPLOAD_DIR),
        "reply_dir": str(REPLY_DIR),
        "timestamp": now_iso(),
    }


@app.post("/api/upload")
async def upload_media(
    file: UploadFile = File(...),
    kind: str = Form("devkit"),
    request_id: str = Form("upload"),
) -> dict:
    ensure_runtime_dirs()

    suffix = Path(file.filename or "upload.bin").suffix or ".bin"
    safe_kind = sanitize_name(kind, "devkit")
    safe_request = sanitize_name(request_id, "upload")
    filename = sanitize_name(f"{safe_request}_{safe_kind}{suffix}", f"{safe_request}{suffix}")
    destination = (UPLOAD_DIR / filename).resolve()

    try:
        destination.relative_to(UPLOAD_DIR)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail="Invalid upload path.") from exc

    data = await file.read()
    destination.write_bytes(data)

    return {
        "ok": True,
        "filename": filename,
        "bytes": len(data),
        "url": public_url_for(f"uploads/devkit/{filename}"),
    }


@app.post("/api/voice-request")
async def handle_voice_request(request: VoiceRequest) -> dict:
    ensure_runtime_dirs()

    audio_bytes: Optional[bytes] = None
    if request.audio_url:
        try:
            audio_bytes = await read_url_bytes(request.audio_url)
        except Exception as exc:
            raise HTTPException(status_code=502, detail=f"Unable to fetch audio_url: {exc}") from exc

    client = openai_client()
    transcript_text = ""
    is_direct_notice = request.mode in {"notice", "tts"} and bool(request.text_prompt)

    if is_direct_notice:
        answer_text = (request.text_prompt or "").strip()
    elif client is None:
        answer_text = build_mock_answer(request, transcript_text)
    else:
        try:
            transcript_text = transcribe_audio(client, audio_bytes, request)
            answer_text = reason_over_request(client, request, transcript_text)
        except Exception as exc:
            raise HTTPException(status_code=502, detail=f"OpenAI processing failed: {exc}") from exc

    reply_filename = sanitize_name(f"{request.request_id}.wav", "reply.wav")
    reply_path = (REPLY_DIR / reply_filename).resolve()
    try:
        reply_path.relative_to(REPLY_DIR)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail="Invalid reply filename.") from exc

    if client is None:
        write_mock_wav(reply_path)
    else:
        try:
            synthesize_speech(client, answer_text, reply_path)
        except Exception as exc:
            raise HTTPException(status_code=502, detail=f"TTS generation failed: {exc}") from exc

    try:
        audio_info = normalize_reply_wav(reply_path)
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"Reply WAV normalization failed: {exc}") from exc

    return {
        "ok": True,
        "request_id": request.request_id,
        "text": answer_text,
        "audio_url": public_url_for(f"audio_reply/{reply_filename}"),
        "format": "wav",
        "sample_rate_hz": audio_info["sample_rate_hz"],
        "audio_info": audio_info,
        "transcript": transcript_text,
        "mock_mode": client is None,
        "timestamp": now_iso(),
    }
