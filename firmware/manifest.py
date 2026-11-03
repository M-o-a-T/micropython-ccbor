# Standard firmware with ccbor: the port's usual frozen modules, plus
# the ccbor package (the Python half of the module) so that registering
# tags and ccbor.ALoader work out of the box.
# Used with FROZEN_MANIFEST=<this file> (see the top-level Makefile).
include("$(PORT_DIR)/boards/manifest.py")
package("ccbor", base_path="../python")
