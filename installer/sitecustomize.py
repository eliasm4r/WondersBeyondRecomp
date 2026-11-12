import os
import sys
from pathlib import Path

if not Path(sys.executable).stem.lower().startswith("python"):
    import setup

    os._exit(setup.main())
