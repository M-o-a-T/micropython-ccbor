# Unix port with ccbor: the port's usual frozen modules plus ccbor_aio.py,
# so that ccbor.ALoader works as it does on the boards.
include("$(PORT_DIR)/variants/manifest.py")
module("ccbor_aio.py", base_path="../python")
