# Fork decoder baseline without NV16 10-bit support

These normalized decoder snapshots are selected only when the Meson-generated
`config.h` does not define `HAVE_NV16_10LE40`. Both current CI suites produce
this configuration and produce byte-identical snapshots.

The Radxa 1.14-4 decoder goldens remain unchanged and are still selected for the
reference configuration where `HAVE_NV16_10LE40` is defined. The parity script
also checks the inspected caps directly against the generated feature macros, so
selecting a variant cannot hide an incorrectly advertised or omitted 10-bit
format.
