"""Exercise the controlled English grammar in Earley and CYK outputs.

Run after building ParseEngine:
    python tests/english_syntax.py --langs python,cpp17,rust,csharp
"""

import argparse
import importlib.util
import sys

from chart_modes import ROOT, build, check_output, command


DEMO_SENTENCE = "the teacher made the student happy"
DEMO_EXPECTED = (ROOT / "example" / "english-syntax.expected").read_text(encoding="utf-8")


POSITIVE = (
    ("Tom read the book", "transitive", 1),
    ("the young teacher read the old book", "ADJ(young)", 1),
    ("Tom quickly read the book", "ADV(quickly)", 1),
    ("she is a teacher", "complement", 1),
    ("the teacher made the student happy", "V_CAUSE(made)", 1),
    ("the teacher gave the student a book", "V_DITR(gave)", 1),
    ("please open the window", "imperative", 1),
    ("do not open the window", "NOT(not)", 1),
    ("if the girl were happy Tom would help her", "counterfactual", 1),
    ("if Tom found the book Mary would help him", "V_PAST(found)", 1),
    ("Tom wishes that the girl were happy", "wish_sentence", 1),
    ("the girl who read the book smiled", "relative_clause", 1),
    ("the book that Tom read is old", "relative_clause", 1),
    ("Tom believes that the girl arrived", "V_REPORT(believes)", 1),
    ("because the girl arrived Tom smiled", "adverb_clause", 1),
    ("Tom smiled when the girl arrived", "adverb_clause", 1),
    ("the man saw the girl with the telescope", "pp", 2),
    ("Tom read the book and Mary smiled", "CONJ(and)", 1),
)

NEGATIVE = (
    "please the window",
    "Tom believes that",
    "if the girl happy Tom would help her",
    "Tom would the book",
    "the young read the book",
)


def python_parser(mode):
    _, out = build("english-syntax", mode, "python")
    spec = importlib.util.spec_from_file_location(f"english_syntax_{mode}", out / "parser.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, out


def check_python(mode):
    module, out = python_parser(mode)
    check_output([sys.executable, out / "main.py"], out, DEMO_SENTENCE,
                 "解析成功，共 1 个结果", exact=DEMO_EXPECTED)
    for sentence, required_node, count in POSITIVE:
        engine = module.Parser()
        status = engine.run_glr_tree(sentence)
        actual = engine.glr_result_count()
        if status != module.STATUS_OK or actual != count:
            raise AssertionError(f"{mode}: {sentence!r}: status={status}, results={actual}")
        trees = [engine.glr_tree(index).to_string() for index in range(actual)]
        if not all(required_node in tree for tree in trees):
            raise AssertionError(f"{mode}: {sentence!r}: missing {required_node!r}\n{trees}")
    if mode != "glr":
        for sentence in NEGATIVE:
            engine = module.Parser()
            if engine.run_glr_tree(sentence) == module.STATUS_OK:
                raise AssertionError(f"{mode} accepted invalid sentence: {sentence!r}")


def check_compiled(mode, lang):
    runner, out = build("english-syntax", mode, lang)
    check_output(runner, out, DEMO_SENTENCE, "解析成功，共 1 个结果", exact=DEMO_EXPECTED)
    for sentence, _, count in POSITIVE:
        result = command(runner, cwd=out, input_text=sentence + "\n")
        expected = f"解析成功，共 {count} 个结果"
        if result.returncode != 0 or expected not in result.stdout:
            raise AssertionError(f"{lang}/{mode}: {sentence!r}\n{result.stdout}\n{result.stderr}")
    for sentence in NEGATIVE:
        result = command(runner, cwd=out, input_text=sentence + "\n")
        if result.returncode == 0:
            raise AssertionError(f"{lang}/{mode} accepted invalid sentence: {sentence!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--langs", default="python,cpp17,rust,csharp")
    args = parser.parse_args()
    langs = args.langs.split(",")
    if "python" in langs:
        for mode in ("earley", "cyk", "glr"):
            check_python(mode)
    for mode in ("earley", "cyk"):
        for lang in langs:
            if lang in ("cpp17", "rust", "csharp"):
                check_compiled(mode, lang)
    print(f"English syntax checks passed: {', '.join(langs)}")


if __name__ == "__main__":
    main()
