import re

REG = re.compile(r"\bc\.r\[(\d+)\]")
WRITE = re.compile(r"\bc\.r\[(\d+)\]\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)")
LEAVE = re.compile(
    r"(?:\bf_\w+\(c\);"
    r"|\bwp::(?:call|unresolved_jump|bad_resume|unsupported_instruction|illegal_instruction|trap|system_call"
    r"|save_context|resume_context|save_jump|resume_jump|set_decrementer|locked_cache_dma)\(c\b[^;]*\);)"
)
POLL = "WP_POLL(c);"
RETURN = re.compile(r"\breturn;")
INDEXED = "c.r[k]"
WRITE_INDEXED = "c.r[k] ="
FIRST = re.compile(r"\bk = (\d+);")
SETJMP = "setjmp("


def cache_registers(body):
    used, written = set(), set()
    for line in body:
        used.update(int(n) for n in REG.findall(line))
        written.update(int(n) for n in WRITE.findall(line))
        if INDEXED in line:
            first = int(FIRST.search(line).group(1))
            used.update(range(first, 32))
            if WRITE_INDEXED in line:
                written.update(range(first, 32))
    if not used or any(SETJMP in line for line in body):
        return [], body
    used = sorted(used)
    spill = " ".join(f"c.r[{n}] = r{n};" for n in sorted(written))
    reload = " ".join(f"r{n} = c.r[{n}];" for n in used)

    def wrap(statement, then_return=False):
        parts = [spill, statement, "return;" if then_return else reload]
        return "{ " + " ".join(part for part in parts if part) + " }"

    out = []
    skip = False
    for index, line in enumerate(body):
        if skip:
            skip = False
            continue
        indent = line[: len(line) - len(line.lstrip())]
        text = line.strip()
        if text.endswith(":") and not text.startswith(("case ", "default")):
            out.append(line)
            continue
        if INDEXED in text:
            out.append(indent + wrap(REG.sub(lambda m: f"r{m.group(1)}", text)))
            continue
        text = REG.sub(lambda m: f"r{m.group(1)}", text)
        if text == POLL:
            out.append(indent + "if (--::wp::g_poll_counter == 0) [[unlikely]] " + wrap("::wp::poll_interrupts(c);"))
            continue
        call = LEAVE.search(text)
        if call:
            following = body[index + 1].strip() if index + 1 < len(body) else ""
            tail = text == call.group(0) and following == "return;"
            replaced = text[: call.start()] + wrap(call.group(0), tail) + text[call.end():]
            out.append(indent + replaced)
            skip = tail
            continue
        if spill and RETURN.search(text):
            text = RETURN.sub("{ " + spill + " return; }", text)
        out.append(indent + text)
    head = [f"    uint32_t {', '.join(f'r{n}' for n in used)};", f"    {reload}"]
    return head, out
