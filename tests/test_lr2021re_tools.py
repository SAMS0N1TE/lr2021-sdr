# SPDX-License-Identifier: MIT
import os, sys, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, 'third_party', 'lr2021-re', 'tools')
try:
    import numpy, scipy
except ImportError:
    print('numpy or scipy not installed: skipped')
    sys.exit(77)
sys.path.insert(0, TOOLS)
suite = unittest.defaultTestLoader.discover(TOOLS, pattern='test_*.py', top_level_dir=TOOLS)
sys.exit(0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1)
