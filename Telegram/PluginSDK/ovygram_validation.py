"""Bounded, stdlib-only input policy shared with the Desktop SDK copy."""

import io
import re
import tokenize

MAX_REQUIREMENT = 200
_VERSION = r"[A-Za-z0-9.*+!_-]+"
_CLAUSE = rf"(==|!=|<=|>=|~=|<|>)\s*{_VERSION}"
REQUIREMENT = re.compile(
    rf"[A-Za-z0-9](?:[A-Za-z0-9._-]*[A-Za-z0-9])?\s*"
    rf"(?:\[\s*[A-Za-z0-9._-]+(?:\s*,\s*[A-Za-z0-9._-]+)*\s*\])?\s*"
    rf"(?:{_CLAUSE}(?:\s*,\s*{_CLAUSE})*)?\Z")


def valid_text(value, multiline=False):
    if not isinstance(value, str):
        return False
    try:
        value.encode("utf-8", "strict")
    except UnicodeError:
        return False
    allowed = "\r\n\t" if multiline else ""
    return not any((ord(char) < 32 and char not in allowed) or ord(char) == 127 for char in value)


def valid_requirement(value):
    return (valid_text(value) and 0 < len(value) <= MAX_REQUIREMENT
            and REQUIREMENT.fullmatch(value.strip()) is not None)


def preflight_source(source):
    """Limit token count, nesting and unary chains before constructing an AST."""
    count = depth = unary = 0
    for token in tokenize.tokenize(io.BytesIO(source).readline):
        count += 1
        if count > 50000:
            raise ValueError("Слишком сложный исходный файл: больше 50 000 токенов.")
        if token.type == tokenize.OP:
            if token.string in ("(", "[", "{"):
                depth += 1
            elif token.string in (")", "]", "}"):
                depth -= 1
            unary = unary + 1 if token.string in ("+", "-", "~") else 0
            if depth > 100 or unary > 128:
                raise ValueError("Слишком глубокое или сложное выражение в исходном файле.")
        elif token.type not in (tokenize.NL, tokenize.COMMENT):
            unary = 0


def unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Повторный ключ JSON: " + key)
        result[key] = value
    return result
