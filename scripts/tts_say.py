#!/usr/bin/env python3
"""Linux 离线中文语音合成：把一句中文合成成 WAV。终端的语音后端每句调用一次。

    <语音目录>/venv/bin/python scripts/tts_say.py --model zh_CN-chaowen-medium.onnx \\
        --output out.wav --length-scale 0.8 -- 敌方三号步兵残血

声音用 Piper 的「超文」。拼音不用 Piper 自带的转换：它把「阵地」读成 zhen4 de5、
「了」读成 liao3、「六号」读成 lu4 hao2，改用 pypinyin。数字先换成汉字，否则
「3 号」里的 3 会被直接跳过。安装见 scripts/setup_voice.sh。
"""

from __future__ import annotations

import argparse
import re
import sys
import wave

import numpy as np
from piper import PiperVoice, SynthesisConfig
from pypinyin import Style, lazy_pinyin

# 与 piper.phonemize_chinese 的声母表一致。不直接 import 那个模块：它在导入时就拉起
# g2pW（要 transformers 和一个 113 MB 的模型），而这里根本用不到。
_INITIALS = ["zh", "ch", "sh", "b", "p", "m", "f", "d", "t", "n", "l", "g", "k", "h",
             "j", "q", "x", "r", "z", "c", "s", "y", "w"]
_DIGITS = "零一二三四五六七八九"
_CLAUSE = re.compile(r"[，。！？；、,.!?;:：\s]+")
_PAUSE_S = 0.22
# 每个音节（声母、韵母、声调）之后补一个 PAD，与 piper.phonemize_chinese.phonemes_to_ids
# 的分组规则一致；这里只会出现声调这一类分组结尾。
_GROUP_END = {"1", "2", "3", "4", "5"}


def _ids(voice: PiperVoice, phonemes: list[str]) -> list[int]:
    table = voice.config.phoneme_id_map
    ids = list(table["^"])
    for phoneme in phonemes:
        if phoneme not in table:
            continue
        ids.extend(table[phoneme])
        if phoneme in _GROUP_END:
            ids.extend(table["_"])
    return ids + list(table["$"])


def _small_number(n: int) -> str:
    """0..9999 的读法：十二、一百零五、三千。"""
    if n < 10:
        return _DIGITS[n]
    if n < 20:
        return "十" + (_DIGITS[n % 10] if n % 10 else "")
    out, zero = "", False
    for value, unit in ((1000, "千"), (100, "百"), (10, "十"), (1, "")):
        digit = n // value % 10
        if digit:
            if zero:
                out += "零"
            out += _DIGITS[digit] + unit
            zero = False
        elif out:
            zero = True
    return out


def _numbers_to_chinese(text: str) -> str:
    text = re.sub(r"(\d+):(\d{2})", lambda m: f"{int(m.group(1))}分{int(m.group(2))}秒", text)
    return re.sub(r"\d+", lambda m: _small_number(int(m.group())) if len(m.group()) <= 4
                  else "".join(_DIGITS[int(d)] for d in m.group()), text)


def _syllable_phonemes(syllable: str) -> list[str]:
    match = re.fullmatch(r"([a-züv:]+?)([1-5])", syllable)
    if not match:
        return []
    base = match.group(1).replace("u:", "v").replace("ü", "v")
    initial = next((cand for cand in _INITIALS if base.startswith(cand)), "")
    final = base[len(initial):]
    return [initial or "Ø", final, match.group(2)] if final else []


def _pinyin(clause: str) -> list[str]:
    try:
        return lazy_pinyin(clause, style=Style.TONE3, neutral_tone_with_five=True,
                           tone_sandhi=True)
    except TypeError:
        return lazy_pinyin(clause, style=Style.TONE3, neutral_tone_with_five=True)


def synthesize(voice: PiperVoice, text: str, length_scale: float) -> np.ndarray:
    rate = voice.config.sample_rate
    pieces: list[np.ndarray] = []
    for clause in _CLAUSE.split(_numbers_to_chinese(text)):
        phonemes = [p for syllable in _pinyin(clause) for p in _syllable_phonemes(syllable)]
        if not phonemes:
            continue
        audio = voice.phoneme_ids_to_audio(_ids(voice, phonemes),
                                           SynthesisConfig(length_scale=length_scale))
        if pieces:
            pieces.append(np.zeros(int(rate * _PAUSE_S), dtype=np.float32))
        pieces.append(np.asarray(audio, dtype=np.float32).reshape(-1))
    if not pieces:
        raise ValueError("文本里没有可以读的中文")
    audio = np.concatenate(pieces)
    return audio / max(1e-6, float(np.abs(audio).max())) * 0.9


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--length-scale", type=float, default=0.8)
    parser.add_argument("text", nargs="+")
    args = parser.parse_args()

    voice = PiperVoice.load(args.model)
    if voice.config.phoneme_type.value != "pinyin":
        print("这个声音不是拼音声音，tts_say.py 只支持超文这类拼音声音", file=sys.stderr)
        return 2
    try:
        audio = synthesize(voice, " ".join(args.text), args.length_scale)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 3
    with wave.open(args.output, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(voice.config.sample_rate)
        out.writeframes((audio * 32767).astype(np.int16).tobytes())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
