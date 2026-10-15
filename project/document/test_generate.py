#!/usr/bin/env python3
"""验证文档生成的幂等性、片段边界、只读检查与配置错误处理。"""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
import generate


class DocumentGeneratorTests(unittest.TestCase):
    """使用根目录 build/ 下的临时仓库验证生成器行为。"""

    def setUp(self):
        scratch = generate.ROOT / "build" / "document-tests"
        scratch.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.patcher = patch.object(generate, "ROOT", self.root)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)
        (self.root / "template.tmpl").write_text("# {{title}}\n\n{{repo}}README.md\n", encoding="utf-8")
        self.config = self.root / "config.json"

    def configure(self, documents, values=None):
        self.config.write_text(json.dumps({
            "values": values or {"title": "统一说明"},
            "documents": documents,
        }), encoding="utf-8")

    def run_generator(self, check=False):
        with redirect_stdout(io.StringIO()):
            return generate.generate(self.config, check)

    def task(self, output="doc/guide.md", **extras):
        return dict(template="template.tmpl", output=output, **extras)

    def test_generation_is_idempotent_and_links_are_relative(self):
        self.configure([self.task()])
        self.assertEqual(self.run_generator(), 0)
        output = self.root / "doc/guide.md"
        self.assertIn("../README.md", output.read_text(encoding="utf-8"))
        stamp = output.stat().st_mtime_ns
        self.assertEqual(self.run_generator(), 0)
        self.assertEqual(output.stat().st_mtime_ns, stamp)
        self.assertEqual(self.run_generator(check=True), 0)

    def test_check_detects_missing_and_stale_without_writing(self):
        self.configure([self.task()])
        output = self.root / "doc/guide.md"
        self.assertEqual(self.run_generator(check=True), 1)
        self.assertFalse(output.exists())
        self.run_generator()
        output.write_text("手工修改\n", encoding="utf-8")
        before = output.read_bytes()
        self.assertEqual(self.run_generator(check=True), 1)
        self.assertEqual(output.read_bytes(), before)

    def test_two_blocks_preserve_manual_text(self):
        output = self.root / "README.md"
        output.write_text(
            "手写开头\n<!-- docgen:first:start -->\n旧内容\n<!-- docgen:first:end -->\n"
            "手写中间\n<!-- docgen:second:start -->\n旧内容\n<!-- docgen:second:end -->\n手写结尾\n",
            encoding="utf-8",
        )
        self.configure([self.task("README.md", block="first"), self.task("README.md", block="second")])
        self.run_generator()
        content = output.read_text(encoding="utf-8")
        for text in ["手写开头", "手写中间", "手写结尾"]:
            self.assertIn(text, content)
        self.assertEqual(content.count("# 统一说明"), 2)
        self.assertEqual(self.run_generator(check=True), 0)

    def test_invalid_markers_do_not_write_partial_output(self):
        for content in ["无标记", "<!-- docgen:part:end --><!-- docgen:part:start -->",
                        "<!-- docgen:part:start --><!-- docgen:part:start --><!-- docgen:part:end -->"]:
            with self.subTest(content=content):
                output = self.root / "README.md"
                output.write_text(content, encoding="utf-8")
                self.configure([self.task(), self.task("README.md", block="part")])
                with self.assertRaises(ValueError):
                    self.run_generator()
                self.assertFalse((self.root / "doc/guide.md").exists())
                self.assertEqual(output.read_text(encoding="utf-8"), content)

    def test_missing_field_and_nonstring_values_are_rejected(self):
        for values in [{"other": "缺少标题"}, {"title": 3}]:
            with self.subTest(values=values):
                self.configure([self.task()], values)
                with self.assertRaises(ValueError):
                    self.run_generator()
                self.assertFalse((self.root / "doc/guide.md").exists())

    def test_output_conflicts_are_rejected(self):
        for tasks in [[self.task(), self.task()],
                      [self.task(), self.task(block="part")]]:
            with self.subTest(tasks=tasks):
                self.configure(tasks)
                with self.assertRaises(ValueError):
                    self.run_generator()
                self.assertFalse((self.root / "doc/guide.md").exists())

    def test_paths_outside_repository_are_rejected(self):
        for task in [self.task("../escape.md"),
                     dict(template="../escape.tmpl", output="doc/guide.md")]:
            with self.subTest(task=task):
                self.configure([task])
                with self.assertRaises(ValueError):
                    self.run_generator()

    def test_values_are_not_expanded_twice(self):
        self.assertEqual(generate.render("{{title}}", {"title": "示例 {{unknown}}"}),
                         "示例 {{unknown}}\n")


if __name__ == "__main__":
    unittest.main()
