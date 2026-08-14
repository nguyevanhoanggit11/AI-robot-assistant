import os, json, requests

OPENCLAW_URL = os.getenv("OPENCLAW_URL")
OPENCLAW_TOKEN = os.getenv("OPENCLAW_TOKEN")

SYSTEM_PROMPT = """Bạn là robot trợ lý . Luôn trả về tiếng việt có dấu theo dạng JSON đúng format,trong text tuyệt đối không chèn thêm bất kỳ 1 ký tự đặc biệt hay emoji nào.(luôn trả lời lịch sự không được phép sử dụng "mày, "tao"
{"speech": "...", "face": "happy|sad|angry|listening|thinking", "motor": "..."}"""

def ask_openclaw(user_text: str, history: list = None) -> dict:
    messages = [{"role": "system", "content": SYSTEM_PROMPT}]
    if history:
        messages.extend(history)
    messages.append({"role": "user", "content": user_text})

    resp = requests.post(
        OPENCLAW_URL,
        headers={
            "Authorization": f"Bearer {OPENCLAW_TOKEN}",
            "Content-Type": "application/json",
        },
        json={
            "model": "openclaw/default",
            "messages": messages,
        },
        timeout=300,
    )
    resp.raise_for_status()
    content = resp.json()["choices"][0]["message"]["content"]
    print ("open claw client test 10")
    return json.loads(content)