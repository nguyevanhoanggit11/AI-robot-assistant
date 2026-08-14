import asyncio
import websockets
import numpy as np
import io
import wave
import json
import time
import scipy.signal  
import re
import threading
import traceback
from dotenv import load_dotenv

from zipformer_client import transcribe_zipformer
from openclaw_client import ask_openclaw


from vieneu import Vieneu

print("\n[INIT] Đang tải model ViNeu TTS (Chế độ ONNX/CPU Streaming)...")
vieneu_engine = Vieneu(backend="onnx")
print("[INIT] Tải model ViNeu TTS thành công!\n")


# =======================
# CONFIG
# =======================
SERVER_IP   = "0.0.0.0"
SERVER_PORT = 8765

SAMPLE_RATE  = 16000
CHANNELS     = 1
SAMPLE_WIDTH = 2

_DECIM_FACTOR = 3
_LOWPASS_TAPS = scipy.signal.firwin(63, 1.0 / _DECIM_FACTOR)


# =======================
# HELPER FUNCTIONS
# =======================
def pcm_to_wav_bytes(pcm_bytes: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


def check_audio_quality(pcm_bytes: bytes) -> bool:
    if not pcm_bytes or len(pcm_bytes) == 0:
        print("  [AUDIO] CẢNH BÁO: Tín hiệu rỗng do ngắt kết nối hoặc lỗi đường truyền.")
        return False

    samples = np.frombuffer(pcm_bytes, dtype=np.int16)
    duration = len(samples) / SAMPLE_RATE
    rms      = np.sqrt(np.mean(samples.astype(np.float32) ** 2))
    peak     = int(np.max(np.abs(samples)))

    print(f"  [AUDIO] Duration: {duration:.2f}s | RMS: {rms:.1f} | Peak: {peak}")

    if peak > 32000:
        print("  [AUDIO] warning: Audio bị clipping — giảm gain trên ESP32.")
    if duration < 0.3:
        print("  [AUDIO] warning: Audio quá ngắn.")
        return False

    return True


def float_16k_to_int16_bytes(audio_16k: np.ndarray) -> bytes:
    
    # 1. Ép biên độ tránh bị vỡ tiếng (Clipping)
    audio_clipped = np.clip(audio_16k, -1.0, 1.0)

    # 2. Chuyển từ Float32 sang Int16
    audio_int16 = (audio_clipped * 32767).astype(np.int16)

    return audio_int16.tobytes()


# =======================
# WEBSOCKET HANDLER
# =======================
MAX_HISTORY_TURNS = 5

async def handle_audio_stream(websocket):
    client_addr = websocket.remote_address
    print(f"\n[CONNECT] ESP32 kết nối từ {client_addr}")

    audio_buffer = bytearray()
    conversation_history = []  
    try:
        async for message in websocket:

            # --- Nhận binary chunk PCM từ ESP32 ---
            if isinstance(message, bytes):
                audio_buffer.extend(message)
                print(f"  [CHUNK] +{len(message)} bytes | Tổng buffer: {len(audio_buffer)} bytes")

            # --- Nhận text signal từ ESP32 ---
            elif isinstance(message, str):
                cmd = message.strip()

                if cmd == "END":
                    t0 = time.time()
                    print(f"  [END] Buffer: {len(audio_buffer)} bytes = {len(audio_buffer)/32000:.2f}s audio")

                    if not check_audio_quality(bytes(audio_buffer)):
                        await websocket.send(json.dumps({
                            "speech": "(audio không đủ chất lượng)",
                            "face": "neutral",
                            "motor": "idle"
                        }, ensure_ascii=False))
                        audio_buffer.clear()
                        continue

                    # 1. Nhận dạng giọng nói (STT)
                    loop = asyncio.get_event_loop()
                    transcript = await loop.run_in_executor(None, transcribe_zipformer, bytes(audio_buffer))
                    print(f"  [STT] Text nhận dạng: '{transcript}'")

                    # 2. Hỏi LLM (OpenClaw), có kèm lịch sử hội thoại gần nhất
                    if transcript and len(transcript.strip()) > 2:
                        try:
                            result = await loop.run_in_executor(
                                None, ask_openclaw, transcript, conversation_history
                            )
                            # Lưu lượt này vào lịch sử để dùng cho câu hỏi kế tiếp
                            conversation_history.append({"role": "user", "content": transcript})
                            conversation_history.append(
                                {"role": "assistant", "content": json.dumps(result, ensure_ascii=False)}
                            )
                            # Giữ tối đa MAX_HISTORY_TURNS lượt gần nhất (mỗi lượt = 2 message)
                            max_messages = MAX_HISTORY_TURNS * 2
                            if len(conversation_history) > max_messages:
                                conversation_history[:] = conversation_history[-max_messages:]
                        except Exception as e:
                            print(f"  [OPENCLAW ERROR] {e}")
                            result = {
                                "speech": "Xin lỗi, tôi gặp lỗi khi xử lý.",
                                "face": "neutral",
                                "motor": "idle"
                            }
                    else:
                        result = {
                            "speech": "Xin lỗi, tôi không nghe rõ.",
                            "face": "neutral",
                            "motor": "idle"
                        }

                    # 3. Gửi JSON câu trả lời & biểu cảm lên ESP32
                    await websocket.send(json.dumps(result, ensure_ascii=False))
                    print(f"  [LLM] Đã gửi JSON. Thời gian xử lý STT+LLM: {time.time()-t0:.2f}s")

                    # 4. Realtime Streaming TTS
                    speech_text = result.get("speech", "")

                    if speech_text:
                        print(f"  [TTS STREAM] Bắt đầu sinh âm thanh cho: '{speech_text}'")
                        t_tts_start = time.time()
                        chunk_count = 0

                        try:
                            # Cắt câu ngắn
                            sentences = [s.strip() for s in re.split(r'[,.?!;\n]+', speech_text) if s.strip()]

                            # Tạo Queue không giới hạn
                            audio_queue = asyncio.Queue()
                            loop = asyncio.get_event_loop()

                            # Định nghĩa Producer chạy trên Thread ngầm
                            def tts_producer():
                               
                                zi = scipy.signal.lfilter_zi(_LOWPASS_TAPS, 1.0) * 0
                                try:
                                    for sentence in sentences:
                                        for chunk_float32 in vieneu_engine.infer_stream(sentence, voice="Minh Đức"):
                                            filtered, zi = scipy.signal.lfilter(_LOWPASS_TAPS, 1.0, chunk_float32, zi=zi)
                                            audio_16k = filtered[::_DECIM_FACTOR]
                                            pcm_bytes = float_16k_to_int16_bytes(audio_16k)

                                            
                                            sub_chunk_size = 4096
                                            for i in range(0, len(pcm_bytes), sub_chunk_size):
                                                sub_chunk = pcm_bytes[i : i + sub_chunk_size]
                                                loop.call_soon_threadsafe(audio_queue.put_nowait, sub_chunk)
                                except Exception:
                                    print("  [TTS PRODUCER ERROR] Lỗi khi sinh audio, dừng sớm:")
                                    traceback.print_exc()
                                finally:
                                    
                                    loop.call_soon_threadsafe(audio_queue.put_nowait, None)

                            # Chạy producer trên thread riêng để không block event loop
                            threading.Thread(target=tts_producer, daemon=True).start()

                            
                            CHUNK_TIMEOUT_S = 10
                            timed_out = False

                           
                            PRESEND_CHUNKS = 8  # ~8*1024 byte ≈ 256ms audio
                            presend_buffer = []
                            for _ in range(PRESEND_CHUNKS):
                                item = await audio_queue.get()
                                if item is None:
                                    break
                                presend_buffer.append(item)

                            for pcm_data in presend_buffer:
                                await websocket.send(pcm_data)
                                chunk_count += 1
                                if chunk_count == 1:
                                    first_latency = (time.time() - t_tts_start) * 1000
                                    print(f"  [TTS STREAM] ⚡ Frame đầu tiên ra lò sau: {first_latency:.1f} ms!")

                            while True:
                                try:
                                    pcm_data = await asyncio.wait_for(audio_queue.get(), timeout=CHUNK_TIMEOUT_S)
                                except asyncio.TimeoutError:
                                    print(f"  [TTS TIMEOUT] Không nhận được chunk mới sau {CHUNK_TIMEOUT_S}s "
                                          f"(đã gửi {chunk_count} chunks) — ViNeu có thể đang treo. Huỷ luồng.")
                                    timed_out = True
                                    break

                                if pcm_data is None:  # Hết dữ liệu
                                    break


                                # GỬI TRỰC TIẾP BINARY PACKET XUỐNG ESP32
                                
                                await websocket.send(pcm_data)
                                chunk_count += 1
                                print(f"  [TTS STREAM] -> Đã gửi chunk #{chunk_count} ({len(pcm_data)} bytes) xuống ESP32")

                                
                                chunk_duration_s = len(pcm_data) / (SAMPLE_RATE * SAMPLE_WIDTH)
                                await asyncio.sleep(chunk_duration_s * 0.75)

                            
                            await websocket.send("AUDIO_END")
                            total_tts_time = time.time() - t_tts_start
                            status = "TIMEOUT - huỷ giữa chừng" if timed_out else "✅ hoàn tất"
                            print(f"  [TTS STREAM] {status}. Đã stream {chunk_count} chunks. Tổng thời gian: {total_tts_time:.2f}s")

                        except Exception as e:
                            print(f"  [TTS ERROR] {e}")

                    # Dọn dẹp buffer sau khi xử lý xong câu lệnh
                    audio_buffer.clear()

    except websockets.exceptions.ConnectionClosed as e:
        print(f"[DISCONNECT] {client_addr} ngắt kết nối. Code: {e.code}")
    except Exception as e:
        print(f"[ERROR] {client_addr} — {e}")
    finally:
        audio_buffer.clear()
        


# =======================
# MAIN
# =======================
async def main():
   
    print(f"[SERVER] Khởi động WebSocket Server tại ws://{SERVER_IP}:{SERVER_PORT}")
    print(f"[SERVER] Đang chờ ESP32 kết nối...\n")

    async with websockets.serve(
        handle_audio_stream,
        SERVER_IP,
        SERVER_PORT,
        max_size=10 * 1024 * 1024,
        ping_interval=20,
        ping_timeout=60,
    ):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
