#!/usr/bin/env python
# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys
import unittest
import os

# add tools folder to sys.path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SDK_DIR = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.append(os.path.join(SDK_DIR, 'tools', 'tests'))

TEST_MODULES = [
    'oshelpers_test',
    'httpd_test',
    'sdktools_test',
    'sdktools_commands_test',
    'update_nacl_manifest_test',
    'generate_make_test'
]

def main():
  suite = unittest.TestSuite()
  for module_name in TEST_MODULES:
    __import__(module_name)
    module = sys.modules[module_name]
    suite.addTests(unittest.defaultTestLoader.loadTestsFromModule(module))

  result = unittest.TextTestRunner(verbosity=2).run(suite)
  return int(not result.wasSuccessful())

if __name__ == '__main__':
  sys.exit(main())
