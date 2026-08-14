import time
import numpy as np
from scipy.signal import resample_poly
from vieneu import Vieneu

VOICE = "Minh Đức"  

print("[INIT] Ðang t?i model VieNeu-TTS test 1")
vieneu = Vieneu()  # m?c d?nh v3 Turbo, CPU/ONNX int8
print("[INIT] VieNeu-TTS s?n sàng.")


def text_to_pcm_vieneu(text: str, target_sample_rate: int = 16000) -> bytes:
  
    t0 = time.time()
    audio = vieneu.infer(text, voice=VOICE)  # float32 numpy array, 48kHz

    audio_resampled = resample_poly(audio, target_sample_rate, 48000)

     peak = np.max(np.abs(audio_resampled))
    if peak > 0:
        audio_resampled = audio_resampled / peak * 0.9
    audio_int16 = np.clip(audio_resampled * 32767, -32768, 32767).astype(np.int16)

    print(f"  [TIME] VieNeu TTS + resample: {time.time()-t0:.2f}s")
    return audio_int16.tobytes()