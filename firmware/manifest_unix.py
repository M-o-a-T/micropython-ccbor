# Unix port with ccbor: the port's usual frozen modules plus the ccbor
# package (the Python half of the module), so that registering tags and
# ccbor.ALoader work as they do on the boards.
include("$(PORT_DIR)/variants/manifest.py")
package("ccbor", base_path="../python")
