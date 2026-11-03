import base64
import struct
import unittest

from ls_record import BEGIN, frames_of

HEAD = BEGIN.search("tui: rec begin cols 4 rows 2 cw 8 ch 16 font 0 theme Terminal Bay "
                    "daylight 0 rotation 0 seconds 1 baud 2000000")


def le(*px):
    return struct.pack("<%dH" % len(px), *px)


def runs(*px):
    """~P runs, one a pixel."""
    return "~P " + "".join("01%04x" % v for v in px)


def b64(*px):
    return "~B " + base64.b64encode(le(*px)).decode()


def pictures(*lines):
    images = []
    _, _, frames, bad = frames_of(HEAD, "\n".join(lines) + "\n", images)
    return images, frames, bad


class PictureLines(unittest.TestCase):
    def test_whole_picture_in_base64(self):
        images, frames, bad = pictures("~F 0", "~I 0 0 4 2 2 2", b64(1, 2), b64(3, 4), "~E")
        self.assertEqual(0, bad)
        self.assertEqual([(0, 0, 4, 2, 2, 2, le(1, 2, 3, 4))], images)
        self.assertEqual(0, frames[0][3])

    def test_runs_and_base64_mix(self):
        images, _, bad = pictures("~F 0", "~I 0 0 4 2 2 2", runs(1, 2), b64(3, 4), "~E")
        self.assertEqual(0, bad)
        self.assertEqual(le(1, 2, 3, 4), images[0][6])

    def test_patch_mends_a_rectangle(self):
        images, frames, bad = pictures("~F 0", "~I 0 0 4 2 3 2", runs(1, 2, 3, 4, 5, 6), "~E",
                                       "~F 40", "~R 1 0 2 2", b64(7, 8, 9, 10), "~E")
        self.assertEqual(0, bad)
        self.assertEqual(le(1, 7, 8, 4, 9, 10), images[1][6])
        self.assertEqual([0, 1], [f[3] for f in frames])

    def test_patches_after_a_scroll_in_one_frame_make_one_picture(self):
        images, frames, bad = pictures("~F 0", "~I 0 0 4 2 2 2", runs(1, 2, 3, 4), "~E",
                                       "~F 40", "~V 1", runs(5, 6), "~R 0 0 1 1", runs(9),
                                       "~R 1 0 1 1", runs(8), "~E")
        self.assertEqual(0, bad)
        self.assertEqual(2, len(images))
        self.assertEqual(le(9, 8, 5, 6), images[1][6])
        self.assertEqual(1, frames[1][3])

    def test_frame_without_picture_lines_keeps_the_picture(self):
        images, frames, bad = pictures("~F 0", "~I 0 0 4 2 1 1", runs(1), "~E", "~F 40", "~E")
        self.assertEqual((0, 1, 0), (bad, len(images), frames[1][3]))

    def test_block_sent_again_unchanged_adds_no_picture(self):
        images, frames, bad = pictures("~F 0", "~I 0 0 4 2 2 2", runs(1, 2, 3, 4), "~E",
                                       "~F 40", "~R 0 1 2 1", runs(3, 4), "~E")
        self.assertEqual((0, 1, 0), (bad, len(images), frames[1][3]))

    def test_patch_without_picture_is_bad(self):
        images, _, bad = pictures("~F 0", "~R 0 0 1 1", runs(1), "~E")
        self.assertEqual(([], 1), (images, bad))

    def test_patch_outside_the_picture_is_bad(self):
        images, _, bad = pictures("~F 0", "~I 0 0 4 2 2 2", runs(1, 2, 3, 4), "~E",
                                  "~F 40", "~R 1 1 2 1", runs(7, 8), "~E")
        self.assertEqual((1, 1), (bad, len(images)))

    def test_short_patch_is_bad_and_goes_in_padded(self):
        images, _, bad = pictures("~F 0", "~I 0 0 4 2 2 2", runs(1, 2, 3, 4), "~E",
                                  "~F 40", "~R 0 0 2 1", runs(7), "~E")
        self.assertEqual((1, le(7, 0, 3, 4)), (bad, images[-1][6]))

    def test_short_whole_picture_still_takes_the_strips_after_it(self):
        # A line lost on the way: what came stays, so later strips and the
        # board's own resent rows still land where they belong.
        images, frames, bad = pictures("~F 0", "~I 0 0 4 2 2 2", runs(1, 2, 3), "~E",
                                       "~F 40", "~V 1", runs(5, 6), "~E")
        self.assertEqual(1, bad)
        self.assertEqual(le(3, 0, 5, 6), images[-1][6])
        self.assertEqual(1, frames[1][3])

    def test_long_strip_is_bad_and_cut_to_size(self):
        images, _, bad = pictures("~F 0", "~I 0 0 4 2 2 2", runs(1, 2, 3, 4), "~E",
                                  "~F 40", "~V 1", runs(5, 6, 7), "~E")
        self.assertEqual((1, le(3, 4, 5, 6)), (bad, images[-1][6]))

    def test_spoilt_line_keeps_the_rest_of_the_block(self):
        images, _, bad = pictures("~F 0", "~I 0 0 4 2 2 2", runs(1), "~P 01zz", runs(3, 4), "~E")
        self.assertEqual(2, bad)
        self.assertEqual(le(1, 3, 4, 0), images[0][6])

    def test_no_picture_clears_it(self):
        _, frames, bad = pictures("~F 0", "~I 0 0 4 2 1 1", runs(1), "~E", "~F 40", "~N", "~E")
        self.assertEqual((0, -1), (bad, frames[1][3]))


if __name__ == "__main__":
    unittest.main()
