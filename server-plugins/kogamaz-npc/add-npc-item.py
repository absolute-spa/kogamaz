"""Добавляет предмет «NPC» во вкладку Blueprint меню строительства
(server/data/items/all.json).

Новый тип объекта клиент 3.5.25 не нарисует, поэтому «NPC» - это маленький
Text Message с текстом $$npc: ручка, за которую NPC двигают, копируют и
удаляют. Самого NPC - аватар с ником и оружием - на маркере держит плагин
kogamaz-npc, и в строительстве, и в игре. В игре сервер маркер прячет.

Запуск: python add-npc-item.py <путь к all.json>
Повторный запуск ничего не ломает - предмет просто обновится.
"""

import base64
import json
import struct
import sys

NPC_ID = 813           # Blueprint, рядом с Oculus
NPC_CATEGORY = 8
OLD_IDS = {622}        # прежний NPC-«text» во вкладке Logic
NPC_NAME = "NPC"
NPC_TEXT = "$$npc"     # то же ищет NpcPlugin
TEXT_SIZE = 0.05       # обычные надписи на картах 0.1-0.2
TEXT_MSG = 24
TEXT_MSG_ITEM = 10374  # предмет встроенного Text Message


def packed_string(text):
    raw = text.encode("utf-8")
    assert len(raw) < 0x80
    return bytes([len(raw)]) + raw


def npc_data():
    # WorldSerializer.Write(runtime: false): прототипов нет, один объект.
    out = bytearray(struct.pack(">ii", 0, 1))
    out += struct.pack(">iiii", 1, -1, TEXT_MSG_ITEM, TEXT_MSG)
    out += struct.pack(">3f4f3f", 0, 0, 0, 0, 0, 0, 1, 1, 1, 1)
    out += struct.pack(">i", 2)
    out += packed_string("text") + b"\x07" + packed_string(NPC_TEXT)
    out += packed_string("textSize") + b"\x02" + struct.pack(">f", TEXT_SIZE)
    out += struct.pack(">ii", 0, 0)   # связи
    return base64.b64encode(bytes(out)).decode("ascii")


def main(path: str) -> None:
    with open(path, encoding="utf-8") as f:
        catalog = json.load(f)

    npc = {
        "id": NPC_ID,
        "name": NPC_NAME,
        "description": "Bot with a nickname: the launcher opens its settings when you place it",
        "category": NPC_CATEGORY,
        "data": npc_data(),
    }
    ids = OLD_IDS | {NPC_ID}
    catalog["items"] = [item for item in catalog["items"] if item["id"] not in ids] + [npc]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(catalog, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"{path}: NPC item {NPC_ID} written")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
