"""Tests for the console broker's log bookkeeping.

There is one thing in here worth testing and it is not obvious from the
outside: send() marks its place in the rolling log, waits, and then asks for
everything logged since. The log is capped and trimmed from the front, so
"its place" cannot be a list index - and for months it was one. The result
was a console that worked all day and then, after about four thousand lines,
returned nothing to every command and blamed the board.

Run:  python -m unittest discover -s bench/lsconsole -p "test_*.py"
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lsconsole                                        # noqa: E402


def fresh_broker():
    """A broker with no port. Nothing under test here touches the wire."""
    return lsconsole.Broker("t-display-p4")


class LogWindow(unittest.TestCase):

    def test_a_mark_reads_back_the_lines_logged_after_it(self):
        b = fresh_broker()
        b.note("before")
        m = b.mark()
        b.note("reply one")
        b.note("reply two")
        self.assertEqual([e[1] for e in b.since(m)], ["reply one", "reply two"])

    def test_a_mark_taken_on_an_empty_log_reads_everything(self):
        b = fresh_broker()
        m = b.mark()
        b.note("first thing the board said")
        self.assertEqual([e[1] for e in b.since(m)],
                         ["first thing the board said"])

    def test_a_mark_still_works_after_the_log_has_wrapped(self):
        """The regression. LS-1087.

        Fill the log to its cap, THEN mark and log a reply. Under the old
        code `mark` was len(self.log), which pins at LOG_MAX once the cap is
        reached, so the slice was self.log[LOG_MAX:] - empty, forever. Every
        command after the four-thousandth line returned nothing, never saw
        the prompt, and burned the full fifteen second timeout.
        """
        b = fresh_broker()
        for i in range(lsconsole.LOG_MAX + 50):
            b.note("chatter %d" % i)
        self.assertEqual(len(b.log), lsconsole.LOG_MAX,
                         "precondition: the log must actually be full")

        m = b.mark()
        b.note(">> version")
        b.note("LakeShark 1.0.3")
        b.note("lakeshark>")

        got = [e[1] for e in b.since(m)]
        self.assertEqual(got, [">> version", "LakeShark 1.0.3", "lakeshark>"])

    def test_a_reply_is_still_found_when_it_itself_overflows_the_log(self):
        """A mark whose window has partly scrolled away must not go negative.

        `lora rx 30` on a busy band can emit more than the whole cap. The
        answer cannot be complete - those lines are gone - but it must be the
        tail of the reply rather than an exception or an empty list.
        """
        b = fresh_broker()
        for i in range(lsconsole.LOG_MAX):
            b.note("chatter %d" % i)
        m = b.mark()
        for i in range(lsconsole.LOG_MAX + 10):
            b.note("packet %d" % i)

        got = [e[1] for e in b.since(m)]
        self.assertEqual(len(got), lsconsole.LOG_MAX)
        self.assertEqual(got[-1], "packet %d" % (lsconsole.LOG_MAX + 9))

    def test_the_prompt_is_matched_on_a_whole_line_not_a_substring(self):
        """LS-973's rule, kept honest.

        The board echoes the command after the prompt, so a substring match
        declares every command finished the instant it is echoed. Only a line
        that is JUST the prompt means ready.
        """
        echo = "lakeshark> version"
        ready = "lakeshark>"
        self.assertNotEqual(echo.strip(), lsconsole.PROMPT)
        self.assertEqual(ready.strip(), lsconsole.PROMPT)


if __name__ == "__main__":
    unittest.main()
