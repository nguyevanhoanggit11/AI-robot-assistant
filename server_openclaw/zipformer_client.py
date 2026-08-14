import time
import numpy as np
import sherpa_onnx

MODEL_DIR = "./sherpa-onnx-zipformer-vi-30M-int8-2026-02-09"

print("[INIT] Ðang t?i model Zipformer STT Local...")
stt_recognizer = sherpa_onnx.OfflineRecognizer.from_transducer(
    encoder=f"{MODEL_DIR}/encoder.int8.onnx",
    decoder=f"{MODEL_DIR}/decoder.onnx",
    joiner=f"{MODEL_DIR}/joiner.int8.onnx",
    tokens=f"{MODEL_DIR}/tokens.txt",
    num_threads=4,
    sample_rate=16000,
    feature_dim=80,
    decoding_method="greedy_search",
    debug=False
)
print("[INIT] Zipformer STT Local s?n sàng.")


def transcribe_zipformer(pcm_bytes: bytes) -> str:
    t0 = time.time()
    samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32) / 32768.0

    stream = stt_recognizer.create_stream()
    stream.accept_waveform(16000, samples)
    stt_recognizer.decode_stream(stream)
    text = stream.result.text.strip()

    print(f"  [TIME] Zipformer STT: {time.time()-t0:.2f}s")
    return text