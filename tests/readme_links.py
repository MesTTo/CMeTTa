"""Refuse a link in README.md that only resolves inside this repository.

Assumes: it runs from extensions/cmetta, which `make docs` does.
Guarantees: README.md is published twice, here and on the site through
website/extensions/cmetta/index.md's @include, one directory deeper and
beside no file of this repository, so a relative link resolves in one of the
two places only, while a URL or an in-page anchor resolves in both. This
exits nonzero naming every link target outside code that is neither, inline
links and reference definitions alike, and prints one line otherwise. The
site's own build refuses such a link only when its extension is off
VitePress's asset list, so `.h` links failed the docs lane while an
`llms.txt` link published a 404 unflagged [source: vitepress 1.6.4,
dist/node/chunk-D3CUZ4fa.js, treatAsHtml; measured 2026-09-24: README.md
held three relative links and the docs lane named two].
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

text = Path("README.md").read_text(encoding="utf-8")
# Code is not prose: a C sample's table[i](x) is a call, not a link.
text = re.sub(r"(?ms)^(`{3,}|~{3,}).*?^\1", "", text)
text = re.sub(r"`[^`\n]*`", "", text)
targets = re.findall(r"\]\(\s*<?([^)\s>]+)", text)
targets += re.findall(r"(?m)^ {0,3}\[[^\]]+\]:\s*<?([^\s>]+)", text)
# A scheme, RFC 3986 section 3.1, or a fragment of this page.
relative = [t for t in targets if not re.match(r"[A-Za-z][A-Za-z0-9+.-]*:|#", t)]
if relative:
    sys.exit(
        "README.md links what only this repository holds, which its site "
        "page cannot reach; write a URL: " + ", ".join(relative)
    )
print("docs: every link in README.md resolves on the site as it does here")
