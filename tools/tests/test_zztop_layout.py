"""ZZTop page layout contract.

framework.c sizes the shared window body from the rows each page declares in
its struct gt_page initialiser (it never reads the page->rows a create
function assigns). A page that places a gadget on a row beyond its declared
count therefore draws over the status line. These checks keep the initialiser,
the create-time assignment and the highest gt_row() used on each page in step.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GADTOOLS = ROOT / "ZZTop" / "GadTools"


def initialiser_rows(text):
    match = re.search(
        r"struct gt_page gt_page_\w+ = \{(.*?)\n\};", text, re.S)
    if not match:
        return None
    fields = [f.strip() for f in match.group(1).split(",\n")]
    # name, title, summary, flags, gadgets, tail, rows
    return int(fields[6])


class ZZTopLayoutTests(unittest.TestCase):
    def test_declared_rows_cover_every_used_row(self):
        for path in sorted(GADTOOLS.glob("page_*.c")):
            text = path.read_text()
            declared = initialiser_rows(text)
            if declared is None:
                continue
            assigned = re.findall(r"page->rows\s*=\s*(\d+);", text)
            if assigned:
                self.assertEqual(declared, int(assigned[-1]), path.name)
            used = [int(n) for n in re.findall(r"gt_row\((\d+)\)", text)]
            used += [int(n) for n in
                     re.findall(r"gt_add_note\(page,\s*\w+,\s*\w+,\s*(\d+),",
                                text)]
            if used:
                self.assertLess(max(used), declared, path.name)

    def test_framework_reads_page_rows(self):
        text = (GADTOOLS / "framework.c").read_text()
        self.assertIn("gt_pages[page]->rows", text)
        self.assertNotIn("row_step * 10)", text)


if __name__ == "__main__":
    unittest.main()
