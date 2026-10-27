# Standard firmware with ccbor: the port's usual frozen modules, plus
# ccbor_aio.py so that ccbor.ALoader works out of the box.
# Used with FROZEN_MANIFEST=<this file> (see the top-level Makefile).
include("$(PORT_DIR)/boards/manifest.py")
module("ccbor_aio.py", base_path="../python")
