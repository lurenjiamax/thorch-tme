# Thor TME transport source

This is the maintained, adapted source checkout. Modify and commit files here
directly. `PROVENANCE` records the original pinned OnePlus revision and input
hashes; those hashes describe upstream inputs, not the adapted working files.

Build through `../scripts/build-tme-modules.py`. The builder does not download,
generate or rewrite C/header source. Kbuild uses a separate module output
directory (`../build/tme-modules`) via MO. The overlay DTS is maintained here.

Local changes include Android-to-mainline API adaptation, bounded response
handling, and debug-level successful-response logging. Full system suspend,
charging stability and recovery after a real DSP restart remain unverified.
