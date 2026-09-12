#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成符合 Obsidian Properties 规范的 issue frontmatter（YAML 头部元数据）。

依据 Obsidian 官方 Properties 文档（https://help.obsidian.md/Editing+and+formatting/Properties）：
  - frontmatter 是文件最顶端的 ``---`` 包裹 YAML 块，键名唯一；
  - List 类型（含 tags）使用块列表，每项一行 ``- item``；
  - 含特殊字符的文本值须用引号包裹（冒号 / 井号 / ``[[内部链接]]`` 等）；
  - 日期类型格式为 YYYY-MM-DD。

支持三种 type（对应 issues/format 目录下的模板）：
  - bug_report    -> Bug_Format.md
  - todo_feature  -> Feature_Format.md
  - review_report -> Review_Format.md

用法：
  python issue_frontmatter.py --type bug_report                # 交互式填写，结果打印到 stdout
  python issue_frontmatter.py --type bug_report --file x.md    # 交互式填写并插入文件头部（已有 frontmatter 则替换）
  python issue_frontmatter.py --type review_report --yes       # 全部取默认值，无交互
  python issue_frontmatter.py --type bug_report --date 2026-08-29 --tag gc,sema --file x.md --yes
  python issue_frontmatter.py --list                           # 列出支持的类型与字段
"""

import argparse
import re
import sys
from datetime import date, datetime

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

# ---------------------------------------------------------------------------
# 字段 schema：kind 取值 text(单行) / list(枚举或自由列表) / date / links(自动转 [[..]])
# multi=True 表示 list 允许逗号分隔多项；choices 不为空时做枚举校验
# ---------------------------------------------------------------------------

def _schema_bug():
    return [
        {"name": "module", "label": "涉及模块（如 Sema / CodeGen / Parser / Runtime）",
         "kind": "text", "required": True, "default": ""},
        {"name": "sub_module", "label": "子模块 / 定位（可选，如 CallInfer.cpp record 分支）",
         "kind": "text", "required": False, "default": ""},
        {"name": "status", "label": "状态", "kind": "list", "required": True,
         "choices": ["pending_fix", "fixed", "needs_review", "researching"],
         "multi": False, "default": "pending_fix"},
        {"name": "severity", "label": "严重程度", "kind": "list", "required": True,
         "choices": ["critical", "high", "medium", "low"], "multi": False, "default": "medium"},
        {"name": "discover_date", "label": "发现日期（YYYY-MM-DD）",
         "kind": "date", "required": True, "default": today_str()},
        {"name": "related_issues", "label": "关联问题（逗号分隔笔记名，如 bug-13；自动加 [[]]）",
         "kind": "links", "required": False, "default": ""},
        {"name": "tags", "label": "标签（逗号分隔）", "kind": "list", "required": False, "default": ""},
    ]

def _schema_feature():
    return [
        {"name": "kind", "label": "类型", "kind": "list", "required": True,
         "choices": ["new_feature", "refactor", "optimization", "tech_debt", "api_design"],
         "multi": True, "default": "new_feature"},
        {"name": "module", "label": "涉及模块（逗号分隔，可多选）", "kind": "list", "required": False,
         "choices": ["Sema", "CodeGen", "Parser", "Runtime"], "multi": True, "default": "Sema"},
        {"name": "status", "label": "进度", "kind": "list", "required": True,
         "choices": ["planned", "designing", "in_progress", "blocked", "review"],
         "multi": False, "default": "planned"},
        {"name": "priority", "label": "优先级", "kind": "list", "required": True,
         "choices": ["P0", "P1", "P2", "P3"], "multi": False, "default": "P2"},
        {"name": "estimated_effort", "label": "预估工作量", "kind": "list", "required": False,
         "choices": ["XS", "S", "M", "L", "XL"], "multi": False, "default": "M"},
        {"name": "blocked_by", "label": "被谁阻塞（逗号分隔）", "kind": "list", "required": False, "default": ""},
        {"name": "discover_date", "label": "登记日期（YYYY-MM-DD）",
         "kind": "date", "required": True, "default": today_str()},
        {"name": "tags", "label": "标签（逗号分隔）", "kind": "list", "required": False, "default": ""},
    ]

def _schema_review():
    return [
        {"name": "kind", "label": "审查类型", "kind": "list", "required": True,
         "choices": ["plan_review", "code_review"], "multi": False, "default": "plan_review"},
        {"name": "plan_file", "label": "待审查的 Plan 文件名", "kind": "text", "required": True, "default": ""},
        {"name": "reviewer", "label": "审查人 / Agent", "kind": "text", "required": True, "default": "AI Agent"},
        {"name": "status", "label": "裁决", "kind": "list", "required": True,
         "choices": ["approved", "changes_requested", "rejected"], "multi": False, "default": "approved"},
        {"name": "severity", "label": "严重程度", "kind": "list", "required": True,
         "choices": ["critical", "major", "minor"], "multi": False, "default": "minor"},
        {"name": "review_date", "label": "审查日期（YYYY-MM-DD）",
         "kind": "date", "required": True, "default": today_str()},
        {"name": "tags", "label": "标签（逗号分隔，建议含 plan_review）", "kind": "list",
         "required": False, "default": ""},
    ]

SCHEMAS = {
    "bug_report": _schema_bug,
    "todo_feature": _schema_feature,
    "review_report": _schema_review,
}

# ---------------------------------------------------------------------------
# YAML 输出（Obsidian 可解析）
# ---------------------------------------------------------------------------

def today_str():
    return date.today().isoformat()


def valid_date(s):
    if not DATE_RE.match(s):
        return False
    try:
        datetime.strptime(s, "%Y-%m-%d")
        return True
    except ValueError:
        return False


def needs_quote(v):
    """是否需要双引号（Obsidian 官方：含特殊字符 / 内部链接的文本须加引号）。"""
    if v == "":
        return False
    if v != v.strip() or "\n" in v:
        return True
    stripped = v.lstrip()
    if stripped.startswith(("-", "?", ":")) or stripped.startswith("#"):
        return True
    if any(c in v for c in ":#{}[],&*!|>'\"%@`"):
        return True
    if v.lower() in ("true", "false", "null", "yes", "no", "on", "off", "~"):
        return True
    return False


def yaml_str(v):
    """单行文本值：特殊字符加双引号，否则裸写。"""
    if not v:
        return '""'
    if needs_quote(v):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return v


def render_line(key, kind, raw):
    """渲染单个字段，返回 (首行, 后续缩进行列表)。

    date/text 单行内联；list/links 统一块列表（Obsidian List 类型）。
    """
    items = _to_items(raw)
    if kind == "links":
        items = [_linkify(x) for x in items]
    if not items:
        if kind in ("text", "date"):
            return f'{key}: ""', []
        return f"{key}: []", []
    if kind in ("text", "date"):
        return f"{key}: {yaml_str(items[0])}", []
    lines = [f"{key}:"]
    for it in items:
        lines.append(f"  - {yaml_str(it)}")
    return lines[0], lines[1:]


def _to_items(raw):
    """把文本按逗号拆成 list；text 类型保持单元素。"""
    raw = raw.strip()
    if not raw:
        return []
    return [p.strip() for p in raw.split(",") if p.strip()]


def _linkify(s):
    if "[" in s:
        return s
    return "[[" + s + "]]"


def render(pairs):
    """pairs: [(key, kind, raw_value), ...] -> frontmatter 文本。"""
    lines = ["---"]
    for key, kind, raw in pairs:
        first, rest = render_line(key, kind, raw)
        lines.append(first)
        lines.extend(rest)
    lines.append("---")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# 交互 / 收集
# ---------------------------------------------------------------------------

def prompt_field(f, given=None):
    """给定(--date/--tag)优先；否则交互输入并校验。返回 raw 字符串（可为空）。"""
    if given is not None:
        return given
    choices = f.get("choices")
    default = f.get("default", "")
    while True:
        hint = f"可选：{' / '.join(choices)}；" if choices else ""
        prompt = f"{f['label']} [{hint}默认 {default or '空'}]> "
        raw = input(prompt).strip()
        if not raw:
            if default:
                return default
            if f.get("required"):
                print("  [!] 必填字段，不能为空。")
                continue
            return ""
        items = _to_items(raw) if f["kind"] in ("list", "links") else [raw]
        if f["kind"] == "date" and not valid_date(items[0]):
            print("  [!] 日期格式应为 YYYY-MM-DD。")
            continue
        if choices:
            bad = [i for i in items if i not in choices]
            if bad:
                print("  [!] 非法取值：" + ", ".join(bad) + "，可选 " + " / ".join(choices))
                continue
        return raw


def collect(args):
    pairs = []
    for f in SCHEMAS[args.type]():
        if args.yes:
            if f["kind"] == "date":
                raw = args.date if args.date else f["default"]
            else:
                raw = f.get("default", "")
        else:
            given = args.date if f["kind"] == "date" else (args.tag if f["name"] == "tags" else None)
            raw = prompt_field(f, given)
        pairs.append((f["name"], f["kind"], raw))
    return pairs


# ---------------------------------------------------------------------------
# 文件插入
# ---------------------------------------------------------------------------

def insert_frontmatter(path, block):
    try:
        with open(path, "r", encoding="utf-8-sig") as fp:  # utf-8-sig 自动剥离 BOM
            content = fp.read()
    except FileNotFoundError:
        content = ""
    m = re.match(r"^---\n.*?\n---\n?", content, re.S)
    if m:
        content = content[m.end():]
    new = block + "\n"
    if content.strip():
        new += "\n" + content.lstrip("\n")
    with open(path, "w", encoding="utf-8", newline="\n") as fp:  # 写回无 BOM
        fp.write(new)
    return path


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="生成符合 Obsidian Properties 规范的 issue frontmatter")
    ap.add_argument("--type", choices=list(SCHEMAS), help="issue 类型")
    ap.add_argument("--file", help="目标 .md 文件；提供后插入其头部（已有 frontmatter 则替换）")
    ap.add_argument("--date", help="日期字段（YYYY-MM-DD），覆盖全部 date 字段")
    ap.add_argument("--tag", help="tags 字段（逗号分隔），覆盖交互输入")
    ap.add_argument("--yes", action="store_true", help="全部使用默认值，免交互")
    ap.add_argument("--list", action="store_true", help="列出支持的类型与字段")
    args = ap.parse_args()

    if args.list:
        for t, build in SCHEMAS.items():
            print(f"[{t}]")
            for f in build():
                kinds = {"text": "text", "list": "list", "date": "date", "links": "list[links]"}
                print(f"  {f['name']:<20}{kinds[f['kind']]:<10}{f['label']}")
        return

    if not args.type:
        ap.error("缺少 --type（问题类型）")

    if args.date and not valid_date(args.date):
        ap.error(f"--date 应为 YYYY-MM-DD，收到：{args.date}")

    pairs = collect(args)
    block = render(pairs)
    if args.file:
        path = insert_frontmatter(args.file, block)
        print(f"已写入 {path}")
    else:
        print(block)


if __name__ == "__main__":
    main()