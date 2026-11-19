# Copyright The Phorepoc Authors
# SPDX-License-Identifier: GPL-3.0-only

r"""components_phevbox -- the phevbox-additions package's components (merged into core's registry when this repository is built:
PHOREPOC_ADAPTATIONS=<adaptations>:<phevbox-additions>, see the pdk's tools/emitter/paths.py).

  phevboxsvc   the guest additions' service (package only: recipes/sis.sh packs it)
  phevboxtest  the same with a self-test, a boot-test component for a board image build (SSM_TEST=phevboxtest); not packaged
"""
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.join(_HERE, "..", "os", "src", "phevboxsvc")

COMPONENTS = {
    "phevboxsvc": dict(mmp=os.path.join(_SRC, "phevboxsvc.mmp"), package_only=True, sis=True),
    "phevboxtest": dict(mmp=os.path.join(_SRC, "phevboxtest.mmp"), test=True),
}
