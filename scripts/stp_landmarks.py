#!/usr/bin/env python3
"""从官方场地模型 RMUC2027_V1.0.0.stp 读出每个有名字的零件在场地上的范围，换成协议坐标（米）。

    python3 scripts/stp_landmarks.py docs/rules/RMUC2027_V1.0.0.stp > build/stp_landmarks.json

模型是 Creo 导出的装配，零件名写在 NEXT_ASSEMBLY_USAGE_OCCURRENCE 的说明字段里（「红堡垒」「中央高地_侧边隧道挡边」……）。
脚本自己解析 STEP 文本：沿装配树把每一层的 ITEM_DEFINED_TRANSFORMATION 乘起来，零件的范围取它所有顶点
（VERTEX_POINT）的外包盒。不碰曲面参数，所以圆弧鼓出顶点的那一点点算不进去，几毫米的误差。

模型坐标是毫米，红方在 +X；协议坐标原点在红方补给区那一角（cpp/map/field_map.h）：
    x = (14000 - X) / 1000,  y = (9124.5 - Y) / 1000
跑一遍要几分钟、几 GB 内存（文件 650MB）。
"""

import json
import re
import sys
from collections import defaultdict

REF = re.compile(r"#(\d+)")
NUMBER = r"([-+0-9.Ee]+)"
POINT = re.compile(r"CARTESIAN_POINT\('[^']*',\(" + NUMBER + "," + NUMBER + "," + NUMBER + r"\)\)")
DIRECTION = re.compile(r"DIRECTION\('[^']*',\(" + NUMBER + "," + NUMBER + "," + NUMBER + r"\)\)")
# 曲线、曲面、坐标轴这些几何定义里的点不一定在零件上（平面的原点可以在任何地方），不往下走。
STOP = ("PLANE", "CYLINDRICAL_SURFACE", "CONICAL_SURFACE", "SPHERICAL_SURFACE", "TOROIDAL_SURFACE",
        "B_SPLINE", "LINE", "CIRCLE", "ELLIPSE", "SURFACE_OF", "OFFSET_SURFACE", "AXIS2_PLACEMENT",
        "DIRECTION", "VECTOR", "PCURVE", "SURFACE_CURVE", "SEAM_CURVE", "DEFINITIONAL_REPRESENTATION",
        "STYLED_ITEM", "PRESENTATION", "COLOUR", "FILL_AREA", "SURFACE_STYLE", "CURVE_STYLE", "(")


def decode(text: str) -> str:
    return re.sub(r"\\X2\\([0-9A-F]+)\\X0\\", lambda m: bytes.fromhex(m.group(1)).decode("utf-16-be"), text)


def read_entities(path: str) -> dict[int, str]:
    entities: dict[int, str] = {}
    parts: list[str] = []
    in_data = False
    with open(path, encoding="latin-1") as stream:
        for line in stream:
            if not in_data:
                in_data = line.startswith("DATA;")
                continue
            if line.startswith("ENDSEC;"):
                break
            parts.append(line.strip())
            if not line.rstrip().endswith(";"):
                continue
            text = "".join(parts)
            parts.clear()
            eq = text.find("=")
            if text.startswith("#") and eq > 0:
                entities[int(text[1:eq])] = text[eq + 1:]
    return entities


def kind(text: str) -> str:
    paren = text.find("(")
    return text[:paren] if paren > 0 else text[:1]


def refs(text: str) -> list[int]:
    return [int(r) for r in REF.findall(text)]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inverse_rigid(m):
    r = [[m[j][i] for j in range(3)] for i in range(3)]
    t = [-sum(r[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [r[0] + [t[0]], r[1] + [t[1]], r[2] + [t[2]], [0, 0, 0, 1]]


def main() -> int:
    entities = read_entities(sys.argv[1])
    print(f"{len(entities)} entities", file=sys.stderr)

    def point(eid):
        m = POINT.match(entities[eid])
        return tuple(float(v) for v in m.groups())

    def direction(eid, default):
        if eid is None:
            return default
        m = DIRECTION.match(entities[eid])
        x, y, z = (float(v) for v in m.groups())
        n = (x * x + y * y + z * z) ** 0.5
        return (x / n, y / n, z / n)

    def placement(eid):
        args = entities[eid]
        items = re.match(r"AXIS2_PLACEMENT_3D\('[^']*',#(\d+),(#\d+|\$),(#\d+|\$)\)", args).groups()
        origin = point(int(items[0]))
        z = direction(int(items[1][1:]) if items[1] != "$" else None, (0.0, 0.0, 1.0))
        x = direction(int(items[2][1:]) if items[2] != "$" else None, (1.0, 0.0, 0.0))
        dot = sum(a * b for a, b in zip(x, z))
        x = tuple(a - dot * b for a, b in zip(x, z))
        n = sum(a * a for a in x) ** 0.5
        x = tuple(a / n for a in x)
        y = (z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0])
        return [[x[0], y[0], z[0], origin[0]], [x[1], y[1], z[1], origin[1]],
                [x[2], y[2], z[2], origin[2]], [0, 0, 0, 1]]

    pds_of = {}            # 产品定义或 NAUO -> PRODUCT_DEFINITION_SHAPE
    rep_of_pds = {}        # PRODUCT_DEFINITION_SHAPE -> 形状表示
    same_space = defaultdict(set)  # 同一坐标系里互相关联的形状表示
    transform_of_pds = {}  # NAUO 的 PRODUCT_DEFINITION_SHAPE -> (表示1, 表示2, ITEM_DEFINED_TRANSFORMATION)
    nauos = []
    for eid, text in entities.items():
        k = kind(text)
        if k == "PRODUCT_DEFINITION_SHAPE":
            pds_of[refs(text)[-1]] = eid
        elif k == "SHAPE_DEFINITION_REPRESENTATION":
            pds, rep = refs(text)[:2]
            rep_of_pds[pds] = rep
        elif k == "SHAPE_REPRESENTATION_RELATIONSHIP":
            a, b = refs(text)[:2]
            same_space[a].add(b)
            same_space[b].add(a)
        elif k == "CONTEXT_DEPENDENT_SHAPE_REPRESENTATION":
            rel, pds = refs(text)[:2]
            rr = entities[rel]
            r = re.search(r"REPRESENTATION_RELATIONSHIP\('[^']*','[^']*',#(\d+),#(\d+)\)", rr)
            t = re.search(r"REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION\(#(\d+)\)", rr)
            transform_of_pds[pds] = (int(r.group(1)), int(r.group(2)), int(t.group(1)))
        elif k == "NEXT_ASSEMBLY_USAGE_OCCURRENCE":
            m = re.match(r"NEXT_ASSEMBLY_USAGE_OCCURRENCE\('([^']*)','([^']*)','([^']*)',#(\d+),#(\d+)", text)
            nauos.append((eid, decode(m.group(3) or m.group(2)), int(m.group(4)), int(m.group(5))))

    children = defaultdict(list)
    child_set = set()
    for eid, name, parent, child in nauos:
        children[parent].append((eid, name, child))
        child_set.add(child)
    roots = [p for p in children if p not in child_set]

    local_box_cache = {}

    def local_box(pd):
        if pd in local_box_cache:
            return local_box_cache[pd]
        rep = rep_of_pds.get(pds_of.get(pd))
        lo, hi = [float("inf")] * 3, [float("-inf")] * 3
        if rep is not None:
            reps, todo = {rep}, [rep]
            while todo:
                for other in same_space[todo.pop()]:
                    if other not in reps:
                        reps.add(other)
                        todo.append(other)
            seen, stack = set(), []
            for r in reps:
                # 表示的条目在第一个括号列表里;最后一个引用是表示上下文,不走。
                stack.extend(refs(entities[r])[:-1])
            while stack:
                eid = stack.pop()
                if eid in seen:
                    continue
                seen.add(eid)
                text = entities[eid]
                k = kind(text)
                if k == "VERTEX_POINT":
                    p = point(refs(text)[0])
                    for i in range(3):
                        lo[i] = min(lo[i], p[i])
                        hi[i] = max(hi[i], p[i])
                    continue
                if k == "EDGE_CURVE":
                    stack.extend(refs(text)[:2])
                    continue
                if k == "ADVANCED_FACE" or k == "FACE_SURFACE":
                    stack.extend(refs(text)[:-1])
                    continue
                if k.startswith(STOP):
                    continue
                stack.extend(refs(text))
        box = (lo, hi) if lo[0] != float("inf") else None
        local_box_cache[pd] = box
        return box

    def placed(box, m):
        lo, hi = box
        out_lo, out_hi = [float("inf")] * 3, [float("-inf")] * 3
        for cx in (lo[0], hi[0]):
            for cy in (lo[1], hi[1]):
                for cz in (lo[2], hi[2]):
                    p = [sum(m[i][j] * c for j, c in enumerate((cx, cy, cz))) + m[i][3] for i in range(3)]
                    for i in range(3):
                        out_lo[i] = min(out_lo[i], p[i])
                        out_hi[i] = max(out_hi[i], p[i])
        return out_lo, out_hi

    def merge(a, b):
        if a is None:
            return b
        if b is None:
            return a
        return [min(x, y) for x, y in zip(a[0], b[0])], [max(x, y) for x, y in zip(a[1], b[1])]

    identity = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    results = []

    def walk(pd, world, path):
        box = None
        own = local_box(pd)
        if own is not None:
            box = placed(own, world)
        parent_rep = rep_of_pds.get(pds_of.get(pd))
        for nauo, name, child in children.get(pd, []):
            rel = transform_of_pds.get(pds_of.get(nauo))
            local = identity
            if rel is not None:
                rep1, rep2, idt = rel
                a1, a2 = refs(entities[idt])[:2]
                m1, m2 = placement(a1), placement(a2)
                # 关系里哪一个是父装配的表示:子在父里 = 父那一侧的坐标轴 · 子那一侧坐标轴的逆。
                if rep2 == parent_rep or (parent_rep is not None and rep2 in same_space[parent_rep]):
                    local = matmul(m2, inverse_rigid(m1))
                else:
                    local = matmul(m1, inverse_rigid(m2))
            child_box = walk(child, matmul(world, local), path + [name])
            if child_box is not None:
                results.append({"path": "/".join(path + [name]), "name": name, "depth": len(path) + 1,
                                "mm": [child_box[0], child_box[1]]})
            box = merge(box, child_box)
        return box

    for root in roots:
        walk(root, identity, [])

    for item in results:
        (x0, y0, z0), (x1, y1, z1) = item["mm"]
        item["x_m"] = [round((14000 - x1) / 1000, 3), round((14000 - x0) / 1000, 3)]
        item["y_m"] = [round((9124.5 - y1) / 1000, 3), round((9124.5 - y0) / 1000, 3)]
        item["z_mm"] = [round(z0, 1), round(z1, 1)]
        item["mm"] = [[round(v, 1) for v in item["mm"][0]], [round(v, 1) for v in item["mm"][1]]]
    json.dump(results, sys.stdout, ensure_ascii=False, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
