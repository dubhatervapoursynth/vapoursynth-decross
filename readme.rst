Description
===========

DeCross is a spatio-temporal cross color (rainbow) reduction filter.

DeCross must be used right after the source filter, before any field
matching or deinterlacing.

The luma is returned unchanged.

This is a port of the DeCross Avisynth plugin, which is in turn based
on the AviUtl plugin CrossColor.


Usage
=====
::

    decross.DeCross(clip clip, [int thresholdy=30, int noise=60, int margin=1, bint debug=False])


Parameters:
    *clip*
        A clip to process. It must have constant format and dimensions
        and it must be 8..16 bit integer YUV with 4:2:0 or 4:2:2
        subsampling.

    *thresholdy*
        Edge detection threshold. Must be between 0 and the maximum
        sample value of the clip, so 0 and 255 at 8 bit and 0 and 65535
        at 16 bit.

        The value is compared against raw sample values and is not
        scaled by the bit depth, so it has to be adjusted when moving a
        script to a deeper clip.

        Smaller values will detect and filter more edges.

        Default: 30.

    *noise*
        Luma difference threshold. Must be between 0 and the maximum
        sample value of the clip, and like *thresholdy* it is compared
        against raw sample values and is not scaled by the bit depth.

        Smaller values will filter more conservatively.

        Default: 60.

    *margin*
        Expands the edge mask to the left and right by *margin*
        pixels. Must be between 0 and 4.

        Default: 1.

    *debug*
        Instead of filtering the rainbows, show the areas that would
        be filtered.

        Default: False.


Compilation
===========

::

    mkdir build && cd build
    meson ../
    ninja


License
=======

GNU GPL v2, like the Avisynth plugin.
