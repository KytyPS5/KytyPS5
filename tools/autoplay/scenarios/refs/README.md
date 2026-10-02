Reference crops for scenario `until` and `verify` steps.

Make one from a screenshot of the running game:

    tools/autoplay/kyty_autoplay.py shot menu                     # prints the PNG path
    tools/autoplay/kyty_autoplay.py ref <that.png> --box 0.01,0.68,0.22,0.99 \
        --out tools/autoplay/scenarios/refs/radar.png

A step matches when its `box` of a live screenshot, shrunk to 32x32 gray, differs from the reference
by at most `max_diff` (mean absolute difference, 0 = identical, 1 = inverted). The match does not
depend on resolution, but it does depend on the crop covering the same part of the screen.

Choose crops of things that exist only on the screen you want (the minimap shows only in free
gameplay), and avoid animated or time-dependent regions.
