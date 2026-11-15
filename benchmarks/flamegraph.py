#!/usr/bin/env python3
"""
Draws a flame graph from folded stacks.

Usage:
    python3 benchmarks/flamegraph.py stacks.folded flame.svg ["a title"]

A folded line is the frames of one stack from the outermost to the
innermost, separated by semicolons, then a space and how many samples
had that stack:

    event loop;main;EventLoop::run;recv;tcp_recvmsg 42

In the picture each box is a function, and its width is the share of
samples in which it was on the stack. What a box stands on called it. The
order from left to right is alphabetical and means nothing.

Needs nothing outside the standard library.
"""

import html
import sys

WIDTH = 1400
ROW = 17
CHAR = 6.2  # roughly the width of one character at this font size


def read_folded(path):
    # The stacks as a tree: name -> [samples, children].
    root = [0, {}]

    with open(path) as f:
        for line in f:
            stack, _, count = line.rstrip("\n").rpartition(" ")
            if not stack:
                continue
            count = int(count)
            node = root
            node[0] += count
            for frame in stack.split(";"):
                node = node[1].setdefault(frame, [0, {}])
                node[0] += count

    return root


def depth_of(node):
    return 1 + max((depth_of(child) for child in node[1].values()), default=0)


def colour(name):
    # Kernel frames in blue, the rest in warm colours. The shade is taken
    # from the name so that a function looks the same wherever it is.
    shade = sum(name.encode()) % 55

    if name.endswith("_[k]"):
        return f"rgb({90 + shade},{140 + shade},{220})"

    return f"rgb({225 + shade % 30},{120 + shade * 2},{60 + shade})"


def draw(root, title):
    total = root[0]
    rows = depth_of(root) - 1
    height = rows * ROW + 60
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{height}" '
        f'viewBox="0 0 {WIDTH} {height}" font-family="Menlo, Consolas, monospace" font-size="11">',
        f'<rect width="{WIDTH}" height="{height}" fill="#fdfdfd"/>',
        f'<text x="{WIDTH / 2}" y="20" text-anchor="middle" font-size="14">{html.escape(title)}</text>',
        f'<text x="{WIDTH / 2}" y="36" text-anchor="middle" fill="#666">'
        f"{total:,} samples. Blue is the kernel. Hover over a box for its share.</text>",
    ]

    def place(node, x, level):
        for name in sorted(node[1]):
            child = node[1][name]
            width = child[0] / total * (WIDTH - 20)

            # Too thin to see. Its children are thinner still.
            if width >= 0.3:
                y = height - 10 - (level + 1) * ROW
                share = child[0] / total * 100
                label = name[:-4] if name.endswith("_[k]") else name
                out.append(
                    f'<g><title>{html.escape(label)}: {child[0]:,} samples, {share:.2f}%</title>'
                    f'<rect x="{x:.1f}" y="{y}" width="{width:.1f}" height="{ROW - 1}" '
                    f'fill="{colour(name)}" rx="1"/>'
                )
                fits = int((width - 6) / CHAR)
                if fits >= 3:
                    text = label if len(label) <= fits else label[: fits - 2] + ".."
                    out.append(
                        f'<text x="{x + 3:.1f}" y="{y + 12}">{html.escape(text)}</text>'
                    )
                out.append("</g>")
                place(child, x, level + 1)

            x += width

    place(root, 10, 0)
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)

    title = sys.argv[3] if len(sys.argv) == 4 else "Flame graph"

    with open(sys.argv[2], "w") as f:
        f.write(draw(read_folded(sys.argv[1]), title))


if __name__ == "__main__":
    main()
